// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
// Copyright 2026 Ryan Murray

//! The fingerprint sensor over SPI.

use kernel::prelude::*;
use kernel::sync::atomic::{Atomic, Relaxed};
use kernel::time::delay::fsleep;
use kernel::time::Delta;

extern "C" {
    fn sep_sensor_register() -> c_int;
    fn sep_sensor_unregister();
    fn sep_sensor_bound() -> c_int;
    fn sep_sensor_power_line() -> c_int;
    fn sep_sensor_cs_timing_mode() -> c_int;
    fn sep_sensor_power_cycle() -> c_int;
    fn sep_sensor_power_source() -> c_int;
    fn sep_sensor_power(on: c_int) -> c_int;
    fn sep_sensor_xfer(tx: *const c_void, rx: *mut c_void, len: usize) -> c_int;
    fn sep_sensor_xfer_tx(tx: *const c_void, len: usize) -> c_int;
    fn sep_sensor_xfer2(tx: *const c_void, tx_len: usize, rx: *mut c_void, rx_len: usize) -> c_int;
    fn sep_sensor_irq_setup() -> c_int;
    fn sep_sensor_irq_available() -> c_int;
    fn sep_sensor_irq_arm();
    fn sep_sensor_irq_wait(timeout_ms: c_uint) -> c_int;
    fn sep_sensor_firmware_name(index: c_uint) -> *const c_char;
}

/// The calibration blob's name from the board description, or `None` when the
/// sensor node has no `firmware-name` property (the caller then uses a default).
pub(crate) fn firmware_name() -> Option<&'static CStr> {
    firmware_name_at(0)
}

/// The signed root-key container's name from the board description: the
/// second `firmware-name` entry, or `None` when the board gives only the
/// calibration (the caller then uses a default).
pub(crate) fn root_key_firmware_name() -> Option<&'static CStr> {
    firmware_name_at(1)
}

fn firmware_name_at(index: c_uint) -> Option<&'static CStr> {
    // SAFETY: the shim returns NULL or a pointer to a NUL-terminated string
    // owned by the device property and valid for the device's lifetime.
    let ptr = unsafe { sep_sensor_firmware_name(index) };
    if ptr.is_null() {
        return None;
    }
    // SAFETY: a non-NULL return is a valid NUL-terminated C string.
    Some(unsafe { CStr::from_char_ptr(ptr) })
}

// Spi2.
pub(crate) const CONTROLLER_BASE: u64 = 0x3_9b10_8000;
pub(crate) const CHIP_SELECT: u32 = 0;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum CsTiming {
    Software,
    HookBypassed,
    Hardware,
}

impl CsTiming {
    fn from_wire(v: c_int) -> CsTiming {
        match v {
            2 => CsTiming::Hardware,
            1 => CsTiming::HookBypassed,
            _ => CsTiming::Software,
        }
    }

    pub(crate) fn name(self) -> &'static CStr {
        match self {
            CsTiming::Hardware => c"HARDWARE — programmed into the controller, which is what this sensor needs",
            CsTiming::HookBypassed => {
                c"emulated — the controller has a set_cs_timing hook but a GPIO chip select bypasses it"
            }
            CsTiming::Software => c"emulated in software — no set_cs_timing hook, and the sensor does not accept this",
        }
    }

    pub(crate) fn is_hardware(self) -> bool {
        matches!(self, CsTiming::Hardware)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum PowerSource {
    None,
    NodeProperty,
    ChipLine,
    /// Rails switched by the SMC on J700.
    SmcRails,
}

impl PowerSource {
    fn from_wire(v: c_int) -> PowerSource {
        match v {
            1 => PowerSource::NodeProperty,
            2 => PowerSource::ChipLine,
            3 => PowerSource::SmcRails,
            _ => PowerSource::None,
        }
    }

    pub(crate) fn name(self) -> &'static CStr {
        match self {
            PowerSource::None => c"none — no power line was taken",
            PowerSource::NodeProperty => c"the sensor node's own gpios property",
            PowerSource::ChipLine => c"/soc/pinctrl@39b028000 line 122, taken by device-tree node",
            PowerSource::SmcRails => c"the sensor node's SMC-switched rails (apple,smc-power-key)",
        }
    }
}

pub(crate) const POWER_ON_READ_DELAYS_MS: [u32; 4] = [0, 3, 10, 50];

pub(crate) const STATUS_IDENTIFIER: usize = 12;
// Known Mesa sensor revisions, including the J700's 0x3356.
pub(crate) const KNOWN_IDENTIFIERS: [u16; 3] = [0x3352, 0x3356, 0x335e];
static_assert!(STATUS_IDENTIFIER + 2 <= STATUS_LEN);

const CMD_LEN: usize = 7;
const CMD_GET_STATUS: [u8; CMD_LEN] = [0x80, 0x10, 0x00, 0x07, 0x00, 0x00, 0x00];
const CMD_IDLE: [u8; CMD_LEN] = [0x80, 0x30, 0x00, 0x07, 0x00, 0x00, 0x00];
const CMD_START_CAPTURE: [u8; CMD_LEN] = [0x80, 0x40, 0x00, 0x07, 0x00, 0x00, 0x00];
// Stops imaging but keeps the finger detector running: the sensor falls to
// idle (state 0) when the finger is lifted. macOS sends it after the frames of
// a touch are taken and arms the next capture when the sensor reports idle.
const CMD_PAUSE_CAPTURE: [u8; CMD_LEN] = [0x80, 0x41, 0x00, 0x07, 0x00, 0x00, 0x00];
// Precedes the patch blob.
const CMD_SETUP_PATCH_ENABLE: [u8; CMD_LEN] = [0x80, 0x60, 0x00, 0x07, 0x00, 0x00, 0x00];
const CMD_GET_SENSOR_SERIAL: [u8; CMD_LEN] = [0x80, 0x70, 0x00, 0x07, 0x00, 0x00, 0x00];
const CMD_GET_NONCE: [u8; CMD_LEN] = [0x80, 0x35, 0x00, 0x07, 0x00, 0x00, 0x00];

const READ_CMD_LEN: usize = 11;
const CMD_READ_PREFIX: [u8; CMD_LEN] = [0x80, 0x13, 0x00, 0x0b, 0x00, 0x00, 0x00];

const STATUS_XFER_LEN: usize = 23;
const STATUS_AT: usize = 7;
pub(crate) const STATUS_LEN: usize = 16;
static_assert!(STATUS_AT + STATUS_LEN == STATUS_XFER_LEN);

// Offsets index the 16-byte status, not the 23-byte transfer.
// Status flags, status[11]. In the macOS trace of the J700 sensor it reads 0x0c
// while armed or idle with no finger, 0x0e once a finger is on and 0x8e with a
// frame ready: bit 1 is the finger detector, bit 7 a frame.
const STATUS_FLAGS: usize = 11;
const FLAG_FINGER: u8 = 0x02;
static_assert!(STATUS_FLAGS < STATUS_LEN);
const STATUS_STATE: usize = STATUS_AT + 7;
const STATUS_COUNT: usize = STATUS_AT + 12;

static_assert!(STATUS_STATE == 14);
static_assert!(STATUS_COUNT == 19);
static_assert!(STATUS_COUNT + 4 == STATUS_XFER_LEN);
static_assert!(STATUS_STATE >= STATUS_AT && STATUS_STATE < STATUS_AT + STATUS_LEN);
static_assert!(STATUS_COUNT >= STATUS_AT && STATUS_COUNT + 4 <= STATUS_AT + STATUS_LEN);

pub(crate) const STATE_DATA_READY: u8 = 7;
// A finger is on the sensor and it is imaging; frames follow as state 7.
pub(crate) const STATE_READING: u8 = 19;
pub(crate) const STATE_IDLE: u8 = 0;
pub(crate) const STATE_ARMED: u8 = 17;
static_assert!(STATE_ARMED != STATE_READING);
static_assert!(STATE_ARMED != STATE_DATA_READY);
static_assert!(STATE_ARMED != STATE_IDLE);
pub(crate) const STATE_NEEDS_PATCH: u8 = 9;

// status[8], not the state byte status[7].
pub(crate) const STATUS_PATCH_ACK: usize = 8;
pub(crate) const PATCH_ACCEPTED: u8 = 0x29;
static_assert!(STATUS_PATCH_ACK != 7);
static_assert!(STATUS_PATCH_ACK < STATUS_LEN);

pub(crate) const MAX_CAPTURE: usize = 0x10000;

const CRC_LEN: usize = 2;

const FRAME_HEADER: usize = 7;
const FRAME_LEN_AT: usize = 3;
static_assert!(FRAME_LEN_AT + 2 <= FRAME_HEADER);

const TYPE_SESSION: u8 = 0x72;
const TYPE_SEQUENCE: u8 = 0x73;
const TYPE_ENCRYPTED: u8 = 0x55;

pub(crate) const SESSION_SHARE_LEN: usize = 40;
pub(crate) const CHALLENGE_LEN: usize = 64;

const MAX_FRAME_PAYLOAD: usize = CHALLENGE_LEN;
static_assert!(SESSION_SHARE_LEN <= MAX_FRAME_PAYLOAD);

pub(crate) const SESSION_REPLY_LEN: usize = 0x33;
pub(crate) const SESSION_REPLY_AT: usize = 9;
pub(crate) const CHALLENGE_REPLY_LEN: usize = 0x4b;
pub(crate) const CHALLENGE_REPLY_AT: usize = 9;

static_assert!(SESSION_REPLY_AT + SESSION_SHARE_LEN <= SESSION_REPLY_LEN);
static_assert!(CHALLENGE_REPLY_AT + CHALLENGE_LEN <= CHALLENGE_REPLY_LEN);

fn check(rc: c_int) -> Result<()> {
    if rc < 0 {
        Err(Error::from_errno(rc))
    } else {
        Ok(())
    }
}

pub(crate) fn register_driver() -> Result<()> {
    // SAFETY: takes no arguments and is idempotent.
    check(unsafe { sep_sensor_register() })
}

pub(crate) fn unregister_driver() {
    // SAFETY: idempotent, and safe when nothing was ever registered.
    unsafe { sep_sensor_unregister() }
}

pub(crate) fn is_bound() -> bool {
    // SAFETY: reads one pointer for nullness.
    unsafe { sep_sensor_bound() != 0 }
}

#[allow(dead_code)]
pub(crate) fn power_line() -> Option<i32> {
    // SAFETY: no arguments; returns a negative errno when unbound.
    let n = unsafe { sep_sensor_power_line() };
    if n < 0 {
        None
    } else {
        Some(n)
    }
}

pub(crate) fn power_cycle() -> bool {
    // SAFETY: the shim holds the descriptor and applies its delays.
    unsafe { sep_sensor_power_cycle() == 0 }
}

pub(crate) fn cs_timing() -> CsTiming {
    // SAFETY: reads one int.
    CsTiming::from_wire(unsafe { sep_sensor_cs_timing_mode() })
}

pub(crate) fn power_source() -> PowerSource {
    // SAFETY: reads one int.
    PowerSource::from_wire(unsafe { sep_sensor_power_source() })
}

pub(crate) fn power(on: bool) -> bool {
    // SAFETY: the shim holds the descriptor and applies its delays.
    unsafe { sep_sensor_power(if on { 1 } else { 0 }) == 0 }
}

/// Sets up the data-ready interrupt once, while the sensor is idle. Returns
/// true when an interrupt is available for interrupt-driven capture.
pub(crate) fn irq_setup() -> bool {
    // SAFETY: no arguments; idempotent, falls back to -errno when unavailable.
    unsafe { sep_sensor_irq_setup() == 0 }
}

pub(crate) fn irq_available() -> bool {
    // SAFETY: reads one int.
    unsafe { sep_sensor_irq_available() != 0 }
}

/// Clears any stale data-ready signal before a capture.
pub(crate) fn irq_arm() {
    // SAFETY: no arguments; a no-op when no IRQ is configured.
    unsafe { sep_sensor_irq_arm() }
}

/// Waits up to `timeout_ms` for the data-ready line to assert. Returns true if
/// it fired, false on timeout or when no interrupt is configured.
pub(crate) fn irq_wait(timeout_ms: c_uint) -> bool {
    // SAFETY: no pointers; the shim owns the completion and the IRQ line.
    unsafe { sep_sensor_irq_wait(timeout_ms) == 0 }
}

fn command(cmd: &[u8; CMD_LEN]) -> Result<()> {
    let mut rx = [0u8; CMD_LEN];
    // SAFETY: both buffers are `CMD_LEN` bytes and live across the call.
    check(unsafe { sep_sensor_xfer(cmd.as_ptr().cast(), rx.as_mut_ptr().cast(), CMD_LEN) })
}

pub(crate) fn idle() -> Result<()> {
    command(&CMD_IDLE)
}

pub(crate) fn start_capture() -> Result<()> {
    command(&CMD_START_CAPTURE)
}

pub(crate) fn pause_capture() -> Result<()> {
    command(&CMD_PAUSE_CAPTURE)
}

pub(crate) fn setup_patch_enable() -> Result<()> {
    command(&CMD_SETUP_PATCH_ENABLE)
}

fn send_framed(frame_type: u8, payload: &[u8]) -> Result<()> {
    let total = FRAME_HEADER + payload.len();
    if payload.len() > MAX_FRAME_PAYLOAD || total > u16::MAX as usize {
        return Err(EINVAL);
    }

    let mut frame = [0u8; FRAME_HEADER + MAX_FRAME_PAYLOAD];
    frame[0] = 0x80;
    frame[1] = frame_type;
    frame[FRAME_LEN_AT..FRAME_LEN_AT + 2].copy_from_slice(&(total as u16).to_le_bytes());
    frame[FRAME_HEADER..total].copy_from_slice(payload);

    // SAFETY: `frame` is at least `total` bytes and lives across the call; the
    // shim only reads from it and there is no receive buffer.
    check(unsafe { sep_sensor_xfer_tx(frame.as_ptr().cast(), total) })
}

const ENCRYPTED_OVERHEAD: usize = FRAME_HEADER + CRC_LEN;
static_assert!(ENCRYPTED_OVERHEAD == 9);
const ENCRYPTED_TRANSFER_MAX: usize = 0x12b;
static_assert!(ENCRYPTED_TRANSFER_MAX < 0x12c);

pub(crate) struct Geometry {
    declared: usize,
    transfer: usize,
}

impl Geometry {
    // 0x5d.
    pub(crate) const COVERAGE: Geometry = Geometry {
        declared: 0x40,
        transfer: 0x40,
    };
    // 0x5c.
    pub(crate) const OPERATION: Geometry = Geometry {
        declared: 0x40,
        transfer: 0x40,
    };
    pub(crate) const MODULE_CHALLENGE: Geometry = Geometry {
        declared: 0x49,
        transfer: 0x49,
    };

    // 0x6a, transmitted zero-padded past declared.
    pub(crate) const TRANSPARENT: Geometry = Geometry {
        declared: 0x4d,
        transfer: 0x12b,
    };

    pub(crate) const fn payload_capacity(&self) -> usize {
        self.declared - ENCRYPTED_OVERHEAD
    }

    pub(crate) fn declared(&self) -> usize {
        self.declared
    }
}

static_assert!(Geometry::MODULE_CHALLENGE.declared < 0x12c);
static_assert!(Geometry::MODULE_CHALLENGE.declared > ENCRYPTED_OVERHEAD);
static_assert!(Geometry::MODULE_CHALLENGE.transfer >= Geometry::MODULE_CHALLENGE.declared);
static_assert!(Geometry::MODULE_CHALLENGE.transfer <= ENCRYPTED_TRANSFER_MAX);
static_assert!(Geometry::MODULE_CHALLENGE.payload_capacity() == 0x40);
static_assert!(Geometry::COVERAGE.declared < 0x12c);
static_assert!(Geometry::OPERATION.declared < 0x12c);
static_assert!(Geometry::TRANSPARENT.declared < 0x12c);
static_assert!(Geometry::COVERAGE.declared > ENCRYPTED_OVERHEAD);
static_assert!(Geometry::OPERATION.declared > ENCRYPTED_OVERHEAD);
static_assert!(Geometry::TRANSPARENT.declared > ENCRYPTED_OVERHEAD);
static_assert!(Geometry::COVERAGE.transfer >= Geometry::COVERAGE.declared);
static_assert!(Geometry::OPERATION.transfer >= Geometry::OPERATION.declared);
static_assert!(Geometry::TRANSPARENT.transfer >= Geometry::TRANSPARENT.declared);
static_assert!(Geometry::COVERAGE.transfer <= ENCRYPTED_TRANSFER_MAX);
static_assert!(Geometry::OPERATION.transfer <= ENCRYPTED_TRANSFER_MAX);
static_assert!(Geometry::TRANSPARENT.transfer <= ENCRYPTED_TRANSFER_MAX);

fn crc16_ansi(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &byte in data {
        crc ^= u16::from(byte);
        for _ in 0..8 {
            if crc & 1 != 0 {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    crc
}

pub(crate) struct Params(KVec<u8>);

impl Params {
    pub(crate) fn new(blob: KVec<u8>) -> Params {
        Params(blob)
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub(crate) fn len(&self) -> usize {
        self.0.len()
    }
}

impl Drop for Params {
    fn drop(&mut self) {
        for b in self.0.iter_mut() {
            // SAFETY: a valid, uniquely borrowed byte.
            unsafe { core::ptr::write_volatile(b, 0) };
        }
    }
}

pub(crate) enum ParamsError {
    Empty,
    TooLong(usize, usize),
    Transfer(Error),
}

pub(crate) fn send_encrypted_parameters(
    params: &Params,
    geom: &Geometry,
) -> core::result::Result<(), ParamsError> {
    if params.0.is_empty() {
        return Err(ParamsError::Empty);
    }
    let capacity = geom.payload_capacity();
    if params.0.len() > capacity {
        return Err(ParamsError::TooLong(params.0.len(), capacity));
    }

    let declared = geom.declared;
    let transfer = geom.transfer;
    let mut frame = [0u8; ENCRYPTED_TRANSFER_MAX];

    frame[0] = 0x80;
    frame[1] = TYPE_ENCRYPTED;
    // Declared size, not the transferred count; the tail past it is zero padding.
    frame[FRAME_LEN_AT..FRAME_LEN_AT + 2].copy_from_slice(&(declared as u16).to_le_bytes());
    frame[FRAME_HEADER..FRAME_HEADER + params.0.len()].copy_from_slice(&params.0);

    let crc_at = declared - CRC_LEN;
    let crc = crc16_ansi(&frame[..crc_at]);
    frame[crc_at..declared].copy_from_slice(&crc.to_le_bytes());

    // SAFETY: `frame` is `ENCRYPTED_TRANSFER_MAX` bytes, `transfer` is no
    // larger (asserted for every `Geometry`), and it lives across the call; the
    // shim only reads from it and there is no receive buffer.
    check(unsafe { sep_sensor_xfer_tx(frame.as_ptr().cast(), transfer) })
        .map_err(ParamsError::Transfer)
}

pub(crate) const MODULE_REPLY_LENS: [usize; 2] = [0x4b, 0x4f];
static_assert!(MODULE_REPLY_LENS[0] <= MAX_REPLY);
static_assert!(MODULE_REPLY_LENS[1] <= MAX_REPLY);
static_assert!(MODULE_REPLY_LENS[0] != MODULE_REPLY_LENS[1]);

const MODULE_REPLY_ADJUST: usize = 11;
const MODULE_REPLY_AT: usize = 9;
static_assert!(MODULE_REPLY_AT == SESSION_REPLY_AT);
static_assert!(MODULE_REPLY_AT == CHALLENGE_REPLY_AT);
static_assert!(MODULE_REPLY_AT == SENSOR_SERIAL_AT);
static_assert!(MODULE_REPLY_ADJUST == MODULE_REPLY_AT + CRC_LEN);
static_assert!(MODULE_REPLY_LENS[0] > MODULE_REPLY_ADJUST);

pub(crate) fn send_module_challenge(challenge: &Params) -> core::result::Result<(), ParamsError> {
    send_encrypted_parameters(challenge, &Geometry::MODULE_CHALLENGE)
}

pub(crate) fn read_module_reply() -> Result<KVec<u8>> {
    let count = poll_until_ready_any(&MODULE_REPLY_LENS)?;

    let mut buf = [0xffu8; MAX_REPLY];
    read_reply_ready(count, &mut buf)?;

    // Relayed unparsed to the enclave, op 0x32.
    let len = count - MODULE_REPLY_ADJUST;
    let mut out = KVec::new();
    out.extend_from_slice(&buf[MODULE_REPLY_AT..MODULE_REPLY_AT + len], GFP_KERNEL)?;
    Ok(out)
}

fn poll_until_ready_any(accepted: &[usize]) -> Result<usize> {
    for _ in 0..READ_POLL_ATTEMPTS {
        let st = status()?;
        if let Offset12::Available(count) = st.offset12() {
            let count = count as usize;
            if accepted.contains(&count) {
                return Ok(count);
            }
        }
        fsleep(Delta::from_millis(READ_POLL_MS));
    }
    Err(ETIMEDOUT)
}

pub(crate) fn send_session_share(share: &[u8; SESSION_SHARE_LEN]) -> Result<()> {
    drain_stale_frames()?;
    send_framed(TYPE_SESSION, share)
}

pub(crate) fn send_challenge(challenge: &[u8; CHALLENGE_LEN]) -> Result<()> {
    drain_stale_frames()?;
    send_framed(TYPE_SEQUENCE, challenge)
}

static_assert!(FRAME_HEADER + SESSION_SHARE_LEN == 0x2f);
static_assert!(FRAME_HEADER + CHALLENGE_LEN == 0x47);

fn poll_until_ready(expected: usize) -> Result<()> {
    for _ in 0..READ_POLL_ATTEMPTS {
        let st = status()?;
        if let Offset12::Available(count) = st.offset12() {
            if count as usize == expected {
                return Ok(());
            }
        }
        fsleep(Delta::from_millis(READ_POLL_MS));
    }
    Err(ETIMEDOUT)
}

const READ_POLL_MS: i64 = 5;
const READ_POLL_ATTEMPTS: usize = 200;

fn read_reply_ready(total: usize, buf: &mut [u8; MAX_REPLY]) -> Result<()> {
    if total > MAX_REPLY {
        return Err(EINVAL);
    }

    let mut cmd = [0u8; READ_CMD_LEN];
    cmd[..CMD_LEN].copy_from_slice(&CMD_READ_PREFIX);
    cmd[CMD_LEN..].copy_from_slice(&(total as u32).to_le_bytes());

    // SAFETY: `cmd` is `READ_CMD_LEN` bytes, `buf` is `MAX_REPLY` and `total`
    // is bounded by it; both live across the call and the shim writes only into
    // the receive buffer.
    check(unsafe {
        sep_sensor_xfer2(
            cmd.as_ptr().cast(),
            READ_CMD_LEN,
            buf.as_mut_ptr().cast(),
            total,
        )
    })
}

/// Frames the sensor still offered after the read that should have taken
/// them, and stale frames found on offer before a command. Both mean an
/// exchange went wrong on the bus; the caller reports them.
static REREADS: Atomic<u32> = Atomic::new(0);
static DRAINED: Atomic<u32> = Atomic::new(0);

/// Takes the counts of re-read and drained frames since the last call.
pub(crate) fn take_recoveries() -> (u32, u32) {
    (REREADS.xchg(0, Relaxed), DRAINED.xchg(0, Relaxed))
}

// A garbled read the sensor did not take is read once more, twice at most.
const FRAME_READS: usize = 3;
// Stale frames taken before a command; the sensor queues at most a couple.
const DRAIN_FRAMES: usize = 2;

/// A reply buffer, wiped however the read that fills it ends.
struct FrameBuf([u8; MAX_REPLY]);

impl FrameBuf {
    fn new() -> Self {
        FrameBuf([0xff; MAX_REPLY])
    }
}

impl Drop for FrameBuf {
    fn drop(&mut self) {
        for b in self.0.iter_mut() {
            // SAFETY: a valid, uniquely borrowed byte.
            unsafe { core::ptr::write_volatile(b, 0) };
        }
    }
}

fn frame_crc_valid(frame: &[u8]) -> bool {
    let split = frame.len() - CRC_LEN;
    u16::from_le_bytes([frame[split], frame[split + 1]]) == crc16_ansi(&frame[..split])
}

/// Takes any frame the sensor still offers before a new command, so the
/// reply to that command is not mistaken for, or queued behind, a stale one.
/// Fails if the sensor still offers a frame once the budget is spent: a
/// read is no proof the sensor took the frame.
fn drain_stale_frames() -> Result<()> {
    for _ in 0..DRAIN_FRAMES {
        let Some(count) = status()?.available().filter(|n| *n != 0) else {
            return Ok(());
        };
        let count = count as usize;
        if count > MAX_REPLY {
            return Err(EIO);
        }
        let mut buf = FrameBuf::new();
        read_reply_ready(count, &mut buf.0)?;
        DRAINED.fetch_add(1, Relaxed);
    }
    match status()?.available() {
        None | Some(0) => Ok(()),
        Some(_) => Err(EBUSY),
    }
}

/// Sends `cmd` and reads its `total`-byte reply, checking the reply's CRC.
fn command_frame(cmd: &[u8; CMD_LEN], total: usize, at: usize, out: &mut [u8]) -> Result<()> {
    drain_stale_frames()?;
    command(cmd)?;
    read_frame(total, at, out)
}

/// Reads a `total`-byte reply once the sensor offers it and checks its CRC.
/// A read the sensor did not take leaves the frame on offer at the same
/// length: only then is it read again.
fn read_frame(total: usize, at: usize, out: &mut [u8]) -> Result<()> {
    if !(CRC_LEN..=MAX_REPLY).contains(&total) || at + out.len() > total - CRC_LEN {
        return Err(EINVAL);
    }
    poll_until_ready(total)?;

    let mut buf = FrameBuf::new();
    for attempt in 0..FRAME_READS {
        if attempt > 0 {
            if status()?.available() != Some(total as u32) {
                break;
            }
            REREADS.fetch_add(1, Relaxed);
        }
        read_reply_ready(total, &mut buf.0)?;
        if frame_crc_valid(&buf.0[..total]) {
            out.copy_from_slice(&buf.0[at..at + out.len()]);
            return Ok(());
        }
    }
    Err(EIO)
}

const MAX_REPLY: usize = 0x4f;
static_assert!(SESSION_REPLY_LEN <= MAX_REPLY);
static_assert!(CHALLENGE_REPLY_LEN <= MAX_REPLY);
static_assert!(SENSOR_SERIAL_REPLY_LEN <= MAX_REPLY);

pub(crate) const SENSOR_SERIAL_REPLY_LEN: usize = 0x1b;
pub(crate) const SENSOR_SERIAL_LEN: usize = 16;
pub(crate) const SENSOR_SERIAL_AT: usize = 9;
static_assert!(SENSOR_SERIAL_AT == SESSION_REPLY_AT);
static_assert!(SENSOR_SERIAL_AT + SENSOR_SERIAL_LEN <= SENSOR_SERIAL_REPLY_LEN - CRC_LEN);

pub(crate) struct SensorSerial([u8; SENSOR_SERIAL_LEN]);

impl SensorSerial {
    pub(crate) fn bytes(&self) -> &[u8; SENSOR_SERIAL_LEN] {
        &self.0
    }
}

pub(crate) fn read_sensor_serial() -> Result<SensorSerial> {
    let mut serial = [0u8; SENSOR_SERIAL_LEN];
    command_frame(&CMD_GET_SENSOR_SERIAL, SENSOR_SERIAL_REPLY_LEN, SENSOR_SERIAL_AT, &mut serial)?;
    Ok(SensorSerial(serial))
}

/// Read the nonce as one frame; a separate acknowledgment would consume it.
pub(crate) fn session_nonce() -> Result<[u8; 32]> {
    let mut value = [0; 32];
    command_frame(&CMD_GET_NONCE, 9 + value.len() + CRC_LEN, 9, &mut value)?;
    if value.iter().all(|byte| *byte == 0) {
        return Err(EIO);
    }
    Ok(value)
}

pub(crate) fn read_session_reply() -> Result<[u8; SESSION_SHARE_LEN]> {
    let mut share = [0u8; SESSION_SHARE_LEN];
    read_frame(SESSION_REPLY_LEN, SESSION_REPLY_AT, &mut share)?;
    Ok(share)
}

pub(crate) fn read_challenge_reply() -> Result<[u8; CHALLENGE_LEN]> {
    let mut reply = [0u8; CHALLENGE_LEN];
    read_frame(CHALLENGE_REPLY_LEN, CHALLENGE_REPLY_AT, &mut reply)?;
    Ok(reply)
}

pub(crate) fn send_patch(blob: &[u8]) -> Result<()> {
    if blob.is_empty() {
        return Err(EINVAL);
    }
    // SAFETY: `blob` is a live slice for the duration of the call and the shim
    // only reads from it; there is no receive buffer to write into.
    check(unsafe { sep_sensor_xfer_tx(blob.as_ptr().cast(), blob.len()) })
}

pub(crate) struct Status {
    pub(crate) state: u8,
    offset12: u32,
    pub(crate) raw: [u8; STATUS_LEN],
}

pub(crate) enum Offset12 {
    Identifier(u16),
    Available(u32),
    Undefined,
}

pub(crate) struct Identifier(u16);

impl Identifier {
    pub(crate) fn value(&self) -> u16 {
        self.0
    }
}

impl Status {
    /// The length of the frame the sensor offers, if it offers one.
    pub(crate) fn available(&self) -> Option<u32> {
        match self.offset12() {
            Offset12::Available(n) => Some(n),
            _ => None,
        }
    }

    pub(crate) fn offset12(&self) -> Offset12 {
        match self.state {
            STATE_IDLE => Offset12::Identifier(u16::from_le_bytes([
                self.raw[STATUS_IDENTIFIER],
                self.raw[STATUS_IDENTIFIER + 1],
            ])),
            STATE_DATA_READY => Offset12::Available(self.offset12),
            _ => Offset12::Undefined,
        }
    }

    pub(crate) fn identifier_to_register(&self) -> Option<Identifier> {
        match self.offset12() {
            Offset12::Identifier(0) => None,
            Offset12::Identifier(value) => Some(Identifier(value)),
            Offset12::Available(_) | Offset12::Undefined => None,
        }
    }

    pub(crate) fn patch_ack(&self) -> u8 {
        self.raw[STATUS_PATCH_ACK]
    }

    /// The sensor's finger detector sees a finger.
    pub(crate) fn finger_present(&self) -> bool {
        self.raw[STATUS_FLAGS] & FLAG_FINGER != 0
    }

    pub(crate) fn is_silent(&self) -> bool {
        self.raw.iter().all(|b| *b == 0)
    }
}

pub(crate) fn status() -> Result<Status> {
    let mut tx = [0xffu8; STATUS_XFER_LEN];
    tx[..CMD_LEN].copy_from_slice(&CMD_GET_STATUS);
    let mut rx = [0u8; STATUS_XFER_LEN];

    // SAFETY: both buffers are `STATUS_XFER_LEN` bytes and live across the call.
    check(unsafe { sep_sensor_xfer(tx.as_ptr().cast(), rx.as_mut_ptr().cast(), STATUS_XFER_LEN) })?;

    Ok(Status::parse(&rx))
}

impl Status {
    /// Decodes the 23 bytes a GET_STATUS transfer reads back.
    fn parse(rx: &[u8; STATUS_XFER_LEN]) -> Status {
        let mut raw = [0u8; STATUS_LEN];
        raw.copy_from_slice(&rx[STATUS_AT..STATUS_AT + STATUS_LEN]);

        Status {
            state: rx[STATUS_STATE],
            offset12: u32::from_le_bytes([
                rx[STATUS_COUNT],
                rx[STATUS_COUNT + 1],
                rx[STATUS_COUNT + 2],
                rx[STATUS_COUNT + 3],
            ]),
            raw,
        }
    }

    /// No finger is on the sensor: it is idle or armed for a finger, and its
    /// finger detector is clear. After PAUSE_CAPTURE this is the lift. A
    /// sensor that needs its patch counts too, so the capture reports it.
    pub(crate) fn finger_off(&self) -> bool {
        self.state == STATE_NEEDS_PATCH
            || (matches!(self.state, STATE_IDLE | STATE_ARMED) && !self.finger_present())
    }
}

pub(crate) struct Capture(KVec<u8>);

impl Capture {
    pub(crate) fn bytes(&self) -> &[u8] {
        &self.0
    }
}

impl Drop for Capture {
    fn drop(&mut self) {
        for b in self.0.iter_mut() {
            // SAFETY: a valid, uniquely borrowed byte.
            unsafe { core::ptr::write_volatile(b, 0) };
        }
    }
}

pub(crate) enum CaptureError {
    Length(u32),
    Bus(Error),
    NoMemory,
    Checksum { advertised: u16, computed: u16 },
}

pub(crate) fn read_capture(length: u32) -> core::result::Result<Capture, CaptureError> {
    let len = length as usize;
    if length == 0 || len > MAX_CAPTURE || len <= CRC_LEN {
        return Err(CaptureError::Length(length));
    }

    let mut cmd = [0u8; READ_CMD_LEN];
    cmd[..CMD_LEN].copy_from_slice(&CMD_READ_PREFIX);
    cmd[CMD_LEN..].copy_from_slice(&length.to_le_bytes());

    let mut buf = KVec::with_capacity(len, GFP_KERNEL).map_err(|_| CaptureError::NoMemory)?;
    buf.resize(len, 0xff, GFP_KERNEL)
        .map_err(|_| CaptureError::NoMemory)?;

    // SAFETY: `cmd` is `READ_CMD_LEN` bytes and `buf` is `len` bytes; both live
    // across the call, and the shim writes only into the receive buffer.
    let rc = unsafe {
        sep_sensor_xfer2(
            cmd.as_ptr().cast(),
            READ_CMD_LEN,
            buf.as_mut_ptr().cast(),
            len,
        )
    };
    if rc < 0 {
        return Err(CaptureError::Bus(Error::from_errno(rc)));
    }

    let split = len - CRC_LEN;
    let advertised = u16::from_le_bytes([buf[split], buf[split + 1]]);
    // CRC-16/ANSI, not CCITT-FALSE.
    let computed = crc16_ansi(&buf[..split]);
    if advertised != computed {
        return Err(CaptureError::Checksum {
            advertised,
            computed,
        });
    }

    Ok(Capture(buf))
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_sensor_status)]
mod status_tests {
    use super::*;

    // GET_STATUS replies the J700 sensor gave macOS 26.6.2, read from the
    // AppleBiometricSensor unified log of our own traces
    // (traces/touchid/20261107-macos-ident, an LA prompt with three misses
    // and a match; and a Setup Assistant enrolment). Each is paired with what
    // macOS's driver concluded: finger on, or finger off / no finger.
    const fn rx(state: u8, sub: u8, flags: u8, tail: [u8; 4]) -> [u8; STATUS_XFER_LEN] {
        [
            0, 0, 0, 0, 0, 0, 0, 0x80, 0x20, 0, 0x10, 0, 0, 0, state, sub, 0xa1, 0, flags, tail[0],
            tail[1], tail[2], tail[3],
        ]
    }
    const IDENT: [u8; 4] = [0x56, 0x33, 0x02, 0x02];
    const IDLE_LIFTED: [u8; STATUS_XFER_LEN] = rx(0x00, 0x00, 0x0c, IDENT);
    const IDLE_AFTER_IDLE_CMD: [u8; STATUS_XFER_LEN] = rx(0x00, 0x00, 0x5c, IDENT);
    const ARMED_EMPTY: [u8; STATUS_XFER_LEN] = rx(0x11, 0x06, 0x0c, [0, 0, 0, 0]);
    const ARMED_FINGER: [u8; STATUS_XFER_LEN] = rx(0x11, 0x02, 0x0e, [0x80, 0, 0, 0x80]);
    const FINGER_ON: [u8; STATUS_XFER_LEN] = rx(0x13, 0x00, 0x0e, [0, 0, 0, 0]);
    const FRAME_READY: [u8; STATUS_XFER_LEN] = rx(0x07, 0x00, 0x8e, [0, 0x72, 0, 0]);
    const IMAGING: [u8; STATUS_XFER_LEN] = rx(0x16, 0x03, 0x0e, [0x04, 0x06, 0x18, 0x1d]);

    #[test]
    fn decodes_the_statuses_macos_saw() {
        for (bytes, off) in [
            (IDLE_LIFTED, true),
            (IDLE_AFTER_IDLE_CMD, true),
            (ARMED_EMPTY, true),
            (ARMED_FINGER, false),
            (FINGER_ON, false),
            (FRAME_READY, false),
            (IMAGING, false),
        ] {
            let st = Status::parse(&bytes);
            assert_eq!(st.finger_off(), off);
            assert_eq!(st.finger_present(), !off);
        }
        assert_eq!(Status::parse(&FRAME_READY).available(), Some(0x7200));
        assert_eq!(Status::parse(&ARMED_EMPTY).available(), None);
        assert_eq!(Status::parse(&FINGER_ON).state, STATE_READING);
    }

    // Prompt 1 of the 17:32 trace, status by status: three touches, each
    // paused after its frames, then lifted. A lift detector that arms on the
    // first finger_off() after a touch must fire exactly where macOS logged
    // FingerOFF and re-armed (07.042, 08.537, 09.910), never on a frame.
    #[test]
    fn replays_three_back_to_back_touches() {
        let seq = [
            (FINGER_ON, false),   // 06.356 touch 1 lands
            (FRAME_READY, false), // 06.437 .. 06.702 frames (match + tail)
            (FRAME_READY, false),
            (IDLE_LIFTED, true),  // 07.042 lift -> START_CAPTURE
            (ARMED_EMPTY, false), // 07.043 armed, waiting
            (FINGER_ON, false),   // 07.728 touch 2
            (FRAME_READY, false),
            (IDLE_LIFTED, true),  // 08.537 lift
            (ARMED_EMPTY, false),
            (FINGER_ON, false),   // 09.077 touch 3
            (FRAME_READY, false),
            (IDLE_LIFTED, true),  // 09.910 lift
        ];
        let mut touched = false;
        let mut lifts = 0;
        for (bytes, macos_lift) in seq {
            let st = Status::parse(&bytes);
            let lift = touched && st.finger_off();
            assert_eq!(lift, macos_lift);
            if st.finger_present() {
                touched = true;
            }
            if lift {
                touched = false;
                lifts += 1;
            }
        }
        assert_eq!(lifts, 3);
    }

    // Touch ID's lift detector is not the J700's alone. It reads the state
    // byte and the finger bit, nothing that names the part, so a status from
    // any Mesa revision a profile names decodes to the same verdict: 0x3352
    // (M1, M1 Pro/Max, M2, M2 Pro/Max, M3) as well as 0x3356 (J700, J616s,
    // J616c, J713). The idle status carries the revision, and it reads back.
    #[test]
    fn lift_detection_is_the_same_for_every_sensor_revision() {
        for id in KNOWN_IDENTIFIERS {
            let [lo, hi] = id.to_le_bytes();
            let ident = [lo, hi, 0x02, 0x02];
            let idle = Status::parse(&rx(0x00, 0x00, 0x0c, ident));
            assert_eq!(idle.identifier_to_register().map(|i| i.value()), Some(id));
            for (bytes, off) in [
                (rx(0x00, 0x00, 0x0c, ident), true),
                (rx(0x00, 0x00, 0x5c, ident), true),
                (rx(0x00, 0x00, 0x0e, ident), false),
                (rx(0x11, 0x06, 0x0c, [0, 0, 0, 0]), true),
                (rx(0x11, 0x02, 0x0e, [0x80, 0, 0, 0x80]), false),
                (rx(0x13, 0x00, 0x0e, [0, 0, 0, 0]), false),
                (rx(0x07, 0x00, 0x8e, [0, 0x72, 0, 0]), false),
                (rx(STATE_NEEDS_PATCH, 0x00, 0x0c, [0, 0, 0, 0]), true),
            ] {
                assert_eq!(Status::parse(&bytes).finger_off(), off);
            }
        }
    }
}
