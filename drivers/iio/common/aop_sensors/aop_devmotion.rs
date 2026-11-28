// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP device-motion fusion driver
//!
//! The `devmotion6` service reports the AOP fusion engine's orientation as a
//! quaternion. The service has several versioned report layouts; the current
//! layouts all carry four little-endian IEEE-754 singles in X, Y, Z, W order.
//! IIO exposes them in its standard W, X, Y, Z order as signed Q30 values.
//!
//! The report client is open only while someone reads the orientation: while
//! a buffer is enabled, or until direct reads go quiet for a short idle
//! window.

use core::{
    arch::asm,
    ffi::{c_int, c_void},
    marker::PhantomPinned,
    mem, ptr,
};

use kernel::{
    bindings, c_str,
    device::{self, Core},
    dma::Coherent,
    driver, new_delayed_work, new_mutex, new_spinlock, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, AOP},
    sync::{
        aref::ARef,
        atomic::{Acquire, Atomic, Relaxed, Release},
        Arc, Mutex, SpinLock,
    },
    time::{self, delay::fsleep, Delta, Instant, Monotonic},
    types::{ForeignOwnable, Opaque},
    workqueue::{self, impl_has_delayed_work, DelayedWork, Work, WorkItem},
    ThisModule,
};

const SUBTYPE_GET_PROPERTY: u16 = 10;
const SUBTYPE_SET_PROPERTY: u16 = 4;
const SUBTYPE_CLIENT_OPEN: u16 = 25;
const SUBTYPE_CLIENT_CLOSE: u16 = 26;
const PROP_INTERVAL: u32 = 0;
/// Selects the firmware's built-in calibration. devmotion6 has no
/// service-specific calibration packets; this generic write is harmless.
const PROP_CALIBRATION_STATE: u32 = 61;
const CALIBRATION_DEFAULT: u32 = 1;
/// The "unsupported" status some firmware returns for the calibration-state
/// write on this service. The stream does not depend on that property:
/// opening the report client is the operation that arms it.
const STATUS_UNSUPPORTED: u32 = 0xe000_02c7;
const PROP_FASTPATH_PAYLOAD_SIZE: u32 = 81;
/// The default sample rate. The fusion engine refreshes the shared buffer at
/// its own fixed rate (200 Hz on the M4 Pro) whatever interval property 0
/// holds, so the host paces itself: it polls at the requested rate and keeps
/// the client open only while samples are consumed.
const DEFAULT_HZ: u32 = 50;
const MAX_HZ: u32 = 200;
/// How long a direct read waits for the first sample of a new client.
const FIRST_SAMPLE_MS: i64 = 500;
const FIRST_SAMPLE_POLL_MS: i64 = 5;
/// How long the client stays open after the last direct read without a
/// buffer, so polling callers share one running client instead of opening and
/// closing it on every sysfs read.
const DIRECT_IDLE_MS: i64 = 2_000;
/// The work item that closes a direct-read client once reads go quiet. The
/// polling work keeps the default id 0.
const IDLE_WORK: u64 = 1;
const FASTPATH_COMMAND_LEN: usize = 84;
const FASTPATH_HEADER_LEN: usize = 64;
const FASTPATH_PAGE_SIZE: usize = 4096;
const MAX_REPORT_LEN: usize = 108;
const Q_FRAC_BITS: i32 = 30;
/// IEEE-754 single 1.0.
const IDENTITY_W: u32 = 0x3f80_0000;

/// The latest quaternion, in the IIO-defined W, X, Y, Z order.
#[derive(Clone, Copy, Default)]
struct Sample {
    quat: [i32; 4],
    valid: bool,
}

/// One buffer scan. A quaternion is aligned like `IIO_DECLARE_QUATERNION()`,
/// which rounds the scan up to a multiple of its 16 bytes. The IIO core
/// copies all of that, so the tail is a field that is written, not padding.
#[repr(C, align(16))]
struct Scan {
    quat: [i32; 4],
    timestamp: i64,
    pad: u64,
}

impl Scan {
    fn new(quat: [i32; 4], timestamp: i64) -> Self {
        Self {
            quat,
            timestamp,
            pad: 0,
        }
    }
}

fn le_u32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn le_u64(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(data.get(at..at + 8)?.try_into().ok()?))
}

/// Converts a finite IEEE-754 single to signed Q30 without using FP in the
/// kernel. Values outside the representation saturate.
fn f32_to_q30(bits: u32) -> Option<i32> {
    let exp = ((bits >> 23) & 0xff) as i32;
    if exp == 0xff {
        return None;
    }
    if exp == 0 {
        return Some(0);
    }

    let mant = i64::from(bits & 0x7f_ffff | 0x80_0000);
    let shift = exp - 127 - 23 + Q_FRAC_BITS;
    let magnitude = if shift >= 31 {
        i64::MAX
    } else if shift >= 0 {
        mant.checked_shl(shift as u32).unwrap_or(i64::MAX)
    } else if shift <= -63 {
        0
    } else {
        mant >> -shift
    };
    let signed = if bits & 0x8000_0000 != 0 {
        -magnitude
    } else {
        magnitude
    };
    Some(signed.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32)
}

fn decode(report: &[u8]) -> Option<[i32; 4]> {
    // The report length varies with the firmware (the M4 Pro sends 52-byte
    // type 3 reports), so only the quaternion has to fit.
    let offset = match report.first().copied()? {
        2 => 28,
        3 | 4 => 12,
        // Type 1 is a five-byte state report and has no orientation.
        _ => return None,
    };
    let bits = [
        le_u32(report, offset)?,
        le_u32(report, offset + 4)?,
        le_u32(report, offset + 8)?,
        le_u32(report, offset + 12)?,
    ];
    // After the AOP starts the fusion engine it publishes the exact identity
    // quaternion until the filter has its first estimate (about 140 ms on
    // the M4 Pro). A measured attitude is never bit-exactly the identity.
    if bits == [0, 0, 0, IDENTITY_W] {
        return None;
    }
    let [x, y, z, w] = bits.map(f32_to_q30);
    Some([w?, x?, y?, z?])
}

/// What a stream state change requires from the service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SyncAction {
    None,
    /// Open the client and start the reports.
    Start,
    /// Stop the reports and close the client.
    Stop,
}

/// Outcome of checking the direct-read idle deadline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum IdleStep {
    /// No direct-read hold to expire.
    Ignore,
    /// More reads extended the deadline; re-arm the work for this many ms.
    RearmMs(u32),
    /// The deadline passed; close the client if nothing else wants it.
    Expire,
}

/// Who wants the report client.
struct Stream<T> {
    /// A buffer is enabled.
    buffer_on: bool,
    /// The client is held open for recent direct reads until this deadline.
    direct_until: Option<T>,
    /// Opening the client for a direct read failed with this error; direct
    /// reads return it without another try until this deadline.
    failed_until: Option<(T, Error)>,
    /// Set across system sleep and after unbind: no client may be open.
    parked: bool,
    /// The client was opened and the reports started.
    active: bool,
}

impl<T> Stream<T> {
    const fn new() -> Self {
        Self {
            buffer_on: false,
            direct_until: None,
            failed_until: None,
            parked: false,
            active: false,
        }
    }

    fn want(&self) -> bool {
        !self.parked && (self.buffer_on || self.direct_until.is_some())
    }

    fn next_action(&self) -> SyncAction {
        match (self.want(), self.active) {
            (true, false) => SyncAction::Start,
            (false, true) => SyncAction::Stop,
            _ => SyncAction::None,
        }
    }

    /// Evaluates the direct-read idle deadline given the milliseconds left
    /// until it expires (`<= 0` once expired).
    fn check_idle(&mut self, remaining_ms: i64) -> IdleStep {
        if self.direct_until.is_none() {
            return IdleStep::Ignore;
        }
        if self.parked || self.buffer_on {
            self.direct_until = None;
            return IdleStep::Ignore;
        }
        if remaining_ms > 0 {
            IdleStep::RearmMs(remaining_ms.clamp(1, DIRECT_IDLE_MS) as u32)
        } else {
            self.direct_until = None;
            IdleStep::Expire
        }
    }

    /// Returns the error of a failed open for as long as direct reads back
    /// off from it, given the milliseconds left until they may try again
    /// (`<= 0` once they may).
    fn check_backoff(&mut self, remaining_ms: i64) -> Option<Error> {
        if remaining_ms <= 0 {
            self.failed_until = None;
        }
        self.failed_until.as_ref().map(|(_, e)| *e)
    }
}

struct Control {
    dev: ARef<device::Device>,
    aop: Arc<dyn AOP>,
    service: EPICService,
}

impl Control {
    fn set(&self, prop: u32, value: u32) -> Result {
        self.set_allowing(prop, value, 0)
    }

    fn set_allowing(&self, prop: u32, value: u32, also_ok: u32) -> Result {
        let mut msg = KVec::with_capacity(12, GFP_KERNEL)?;
        msg.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(&prop.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(&value.to_le_bytes(), GFP_KERNEL)?;
        let status = self
            .aop
            .epic_call(&self.service, SUBTYPE_SET_PROPERTY, &msg)?;
        if status != 0 && (also_ok == 0 || status != also_ok) {
            dev_err!(
                self.dev,
                "property {} refused: status {:#x}\n",
                prop,
                status
            );
            return Err(EIO);
        }
        Ok(())
    }

    fn get(&self, prop: u32, len: usize) -> Result<KVec<u8>> {
        let mut msg = KVec::new();
        msg.resize(8, 0, GFP_KERNEL)?;
        msg[4..8].copy_from_slice(&prop.to_le_bytes());
        let (status, value) =
            self.aop
                .epic_call_ret(&self.service, SUBTYPE_GET_PROPERTY, &msg, len)?;
        if status != 0 {
            dev_err!(
                self.dev,
                "property {} read refused: status {:#x}\n",
                prop,
                status
            );
            return Err(EIO);
        }
        Ok(value)
    }

    /// Writes the generic calibration state. Unsupported is normal for a
    /// service whose actual enable operation is ClientOpen.
    fn calibrate(&self) -> Result {
        self.set_allowing(
            PROP_CALIBRATION_STATE,
            CALIBRATION_DEFAULT,
            STATUS_UNSUPPORTED,
        )
    }

    /// Writes the consumer's interval. The firmware records it, but the
    /// fast-path buffer is refreshed at the fusion rate regardless.
    fn enable(&self, interval_us: u32) -> Result {
        self.set(PROP_INTERVAL, interval_us)
    }

    fn disable(&self) -> Result {
        self.set(PROP_INTERVAL, 0)
    }
}

#[pin_data(PinnedDrop)]
struct State {
    iio: IioPtr,
    control: Arc<Control>,
    payload_len: usize,
    stride: usize,
    #[pin]
    ring: Mutex<Option<Coherent<[u8]>>>,
    #[pin]
    sample: SpinLock<Sample>,
    /// The polling work may run. Follows `Stream::active`; the work reads it
    /// without the stream lock.
    active: Atomic<bool>,
    opened: Atomic<bool>,
    retain_ring: Atomic<bool>,
    client_id: Atomic<u64>,
    next_client_id: Atomic<u64>,
    last_aop_timestamp: Atomic<u64>,
    hz: Atomic<u32>,
    /// Serializes opening and closing the client.
    #[pin]
    stream: Mutex<Stream<Instant<Monotonic>>>,
    #[pin]
    poll_work: DelayedWork<State>,
    /// Closes a direct-read client once reads go quiet.
    #[pin]
    idle_work: DelayedWork<State, IDLE_WORK>,
}

struct IioPtr(*mut bindings::iio_dev);

// SAFETY: The IIO device may be used from any thread. The polling worker only
// calls IIO helpers that are valid from process context.
unsafe impl Send for IioPtr {}
// SAFETY: As above.
unsafe impl Sync for IioPtr {}

impl_has_delayed_work! {
    impl HasDelayedWork<Self> for State { self.poll_work }
    impl HasDelayedWork<Self, IDLE_WORK> for State { self.idle_work }
}

impl State {
    fn publish(&self, data: &[u8]) {
        let Some(quat) = decode(data) else {
            // State-only reports are expected and leave the sample unchanged.
            return;
        };
        *self.sample.lock() = Sample { quat, valid: true };
        // SAFETY: The polling work is disabled before the IIO device is freed.
        let timestamp = unsafe { bindings::iio_get_time_ns(self.iio.0) };
        let scan = Scan::new(quat, timestamp);
        // SAFETY: `scan` contains the complete only data channel followed by
        // the timestamp, and the IIO core copies it synchronously.
        unsafe { bindings::iio_push_to_buffers(self.iio.0, ptr::from_ref(&scan).cast()) };
    }

    fn open_fastpath(&self) -> Result {
        if self.opened.load(Acquire) {
            return Ok(());
        }
        let id = self.next_client_id.fetch_add(1, Relaxed);
        let ring = self.ring.lock();
        let ring = ring.as_ref().ok_or(ENODEV)?;
        let mut msg = KVec::new();
        // EPIC reserves the first word. The 84-byte ClientOpen body is:
        // client id, flags, IOVA, allocation size, payload size, then zeroes.
        msg.resize(4 + FASTPATH_COMMAND_LEN, 0, GFP_KERNEL)?;
        msg[4..12].copy_from_slice(&id.to_le_bytes());
        msg[16..24].copy_from_slice(&ring.dma_address().to_le_bytes());
        msg[24..28].copy_from_slice(&(ring.size() as u32).to_le_bytes());
        msg[28..32].copy_from_slice(&(self.payload_len as u32).to_le_bytes());
        let status =
            match self
                .control
                .aop
                .epic_call(&self.control.service, SUBTYPE_CLIENT_OPEN, &msg)
            {
                Ok(status) => status,
                Err(e) => {
                    // A timed-out open may already have handed the IOVA to the
                    // firmware. Keep the mapping for the rest of the boot.
                    self.retain_ring.store(true, Release);
                    return Err(e);
                }
            };
        if status != 0 {
            dev_err!(
                self.control.dev,
                "fast-path open refused: status {:#x}\n",
                status
            );
            return Err(EIO);
        }
        self.client_id.store(id, Relaxed);
        self.opened.store(true, Release);
        Ok(())
    }

    fn close_fastpath(&self) -> Result {
        if !self.opened.load(Acquire) {
            return Ok(());
        }
        let mut msg = KVec::new();
        msg.resize(12, 0, GFP_KERNEL)?;
        msg[4..12].copy_from_slice(&self.client_id.load(Relaxed).to_le_bytes());
        let status =
            match self
                .control
                .aop
                .epic_call(&self.control.service, SUBTYPE_CLIENT_CLOSE, &msg)
            {
                Ok(status) => status,
                Err(e) => {
                    self.retain_ring.store(true, Release);
                    return Err(e);
                }
            };
        if status != 0 {
            self.retain_ring.store(true, Release);
            dev_err!(
                self.control.dev,
                "fast-path close refused: status {:#x}\n",
                status
            );
            return Err(EIO);
        }
        self.opened.store(false, Release);
        Ok(())
    }

    fn start(self: &Arc<Self>) -> Result {
        self.open_fastpath()?;
        let interval_us = 1_000_000 / self.hz.load(Relaxed);
        if let Err(e) = self.control.enable(interval_us) {
            if self.close_fastpath().is_err() {
                self.retain_ring.store(true, Release);
            }
            return Err(e);
        }
        self.active.store(true, Release);
        self.queue_poll();
        Ok(())
    }

    fn stop(&self) -> Result {
        self.active.store(false, Release);
        let interval = self.control.disable();
        let close = self.close_fastpath();
        // A later client must not be answered with this client's last sample.
        self.sample.lock().valid = false;
        interval.and(close)
    }

    /// Opens or closes the client to match its consumers. The caller holds
    /// the stream lock.
    fn sync_locked(self: &Arc<Self>, s: &mut Stream<Instant<Monotonic>>) -> Result {
        match s.next_action() {
            SyncAction::Start => {
                self.start()?;
                s.active = true;
                Ok(())
            }
            SyncAction::Stop => {
                s.active = false;
                self.stop()
            }
            SyncAction::None => Ok(()),
        }
    }

    fn schedule_idle(self: &Arc<Self>, delay_ms: u32) {
        let _ = workqueue::system_dfl().enqueue_delayed::<Arc<Self>, IDLE_WORK>(
            self.clone(),
            time::msecs_to_jiffies(delay_ms),
        );
    }

    fn stop_idle_sync(self: &Arc<Self>, disable: bool) {
        // SAFETY: `idle_work` is initialized and pinned in this `Arc`. Both
        // helpers cancel pending work and join a running callback; the disable
        // variant also prevents any later requeue.
        let cancelled = unsafe {
            let work = Work::raw_get(DelayedWork::raw_as_work(ptr::addr_of!(self.idle_work)));
            let delayed = kernel::container_of!(work, bindings::delayed_work, work);
            if disable {
                bindings::disable_delayed_work_sync(delayed)
            } else {
                bindings::cancel_delayed_work_sync(delayed)
            }
        };
        if cancelled {
            // SAFETY: A cancelled pending callback owned exactly one `Arc`
            // transferred by `enqueue_delayed`.
            drop(unsafe { Arc::from_raw(Arc::as_ptr(self)) });
        }
    }

    fn set_buffer(self: &Arc<Self>, on: bool) -> Result {
        let mut s = self.stream.lock();
        s.buffer_on = on;
        if on {
            // The buffer takes over a client that direct reads opened.
            s.direct_until = None;
            s.failed_until = None;
        }
        let ret = self.sync_locked(&mut s);
        if on && ret.is_err() {
            // The IIO core does not call predisable after a failed enable.
            s.buffer_on = false;
        }
        ret
    }

    fn set_parked(self: &Arc<Self>, parked: bool) -> Result {
        let ret = {
            let mut s = self.stream.lock();
            s.parked = parked;
            s.failed_until = None;
            if parked {
                s.direct_until = None;
            }
            self.sync_locked(&mut s)
        };
        if parked {
            // The idle work takes the stream lock, so join it without it.
            self.stop_idle_sync(false);
        }
        ret
    }

    fn shutdown(self: &Arc<Self>) -> Result {
        let ret = {
            let mut s = self.stream.lock();
            s.parked = true;
            s.direct_until = None;
            self.sync_locked(&mut s)
        };
        self.stop_idle_sync(true);
        ret
    }

    /// Returns the current orientation. Without a buffer consumer the client
    /// is opened on demand and kept open until direct reads go quiet for
    /// `DIRECT_IDLE_MS`. The stream lock is held across the wait for the first
    /// sample, so the idle work cannot close the client under a reader. A
    /// failed open is not tried again for as long as the idle window; reads in
    /// between return its error.
    fn read_now(self: &Arc<Self>) -> Result<[i32; 4]> {
        let mut s = self.stream.lock();
        if s.parked {
            return Err(EBUSY);
        }
        if let Some((until, _)) = s.failed_until {
            let remaining_ms = (until - Instant::<Monotonic>::now()).as_millis_ceil();
            if let Some(e) = s.check_backoff(remaining_ms) {
                return Err(e);
            }
        }
        let started = !s.active;
        if !s.buffer_on {
            s.direct_until = Some(Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS));
        }
        if let Err(e) = self.sync_locked(&mut s) {
            s.direct_until = None;
            // Polling readers must not repeat a failing open at their rate.
            let until = Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
            s.failed_until = Some((until, e));
            return Err(e);
        }
        if started {
            let mut waited = 0;
            while waited < FIRST_SAMPLE_MS && !self.sample.lock().valid {
                fsleep(Delta::from_millis(FIRST_SAMPLE_POLL_MS));
                waited += FIRST_SAMPLE_POLL_MS;
                self.poll();
            }
            if !s.buffer_on {
                s.direct_until =
                    Some(Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS));
            }
        }
        if !s.buffer_on {
            self.schedule_idle(DIRECT_IDLE_MS as u32);
        }
        let sample = *self.sample.lock();
        if sample.valid {
            Ok(sample.quat)
        } else {
            Err(ENODATA)
        }
    }

    fn queue_poll(self: &Arc<Self>) {
        let period_ms = 1000 / self.hz.load(Relaxed);
        let _ = workqueue::system_dfl()
            .enqueue_delayed::<Arc<Self>, 0>(self.clone(), time::msecs_to_jiffies(period_ms));
    }

    fn poll(&self) {
        let guard = self.ring.lock();
        let Some(ring) = guard.as_ref() else {
            return;
        };
        let base = ring.as_ptr().cast::<u8>();
        // SAFETY: the allocation is at least one page. The firmware publishes
        // one of the two aligned slot offsets in the first word.
        let slot = unsafe { u32::from_le(ptr::read_volatile(base.cast::<u32>())) as usize };
        if slot != FASTPATH_HEADER_LEN && slot != FASTPATH_HEADER_LEN + self.stride {
            return;
        }
        // SAFETY: the barrier touches no memory and orders the slot body after
        // the firmware's selector publication.
        unsafe { asm!("dmb oshld", options(nostack, preserves_flags)) };
        // SAFETY: both allowed slots are aligned and fit in the allocation.
        let (aop_timestamp, len) = unsafe {
            (
                u64::from_le(ptr::read_volatile(base.add(slot).cast::<u64>())),
                u32::from_le(ptr::read_volatile(base.add(slot + 8).cast::<u32>())) as usize,
            )
        };
        if aop_timestamp == 0
            || aop_timestamp == self.last_aop_timestamp.load(Relaxed)
            || len > self.payload_len
            || len > MAX_REPORT_LEN
            || slot + 12 + len > ring.size()
        {
            return;
        }
        let mut report = [0u8; MAX_REPORT_LEN];
        // SAFETY: the bounds above keep the source inside the coherent ring;
        // the published slot is not reused until the other slot is published.
        unsafe {
            ptr::copy_nonoverlapping(base.add(slot + 12), report.as_mut_ptr(), len);
            asm!("dmb oshld", options(nostack, preserves_flags));
        }
        // If publication changed while copying, consume the newer slot on the
        // next tick rather than risk a slot that has already been recycled.
        // SAFETY: as for the first selector read.
        if unsafe { u32::from_le(ptr::read_volatile(base.cast::<u32>())) as usize } != slot {
            return;
        }
        self.last_aop_timestamp.store(aop_timestamp, Relaxed);
        self.publish(&report[..len]);
    }

    fn disable_poll(self: &Arc<Self>) {
        self.active.store(false, Release);
        // SAFETY: `poll_work` is initialized and pinned in this `Arc`. The
        // call prevents requeue, cancels pending work and joins a callback.
        let cancelled = unsafe {
            let work = Work::raw_get(DelayedWork::raw_as_work(ptr::addr_of!(self.poll_work)));
            let delayed = kernel::container_of!(work, bindings::delayed_work, work);
            bindings::disable_delayed_work_sync(delayed)
        };
        if cancelled {
            // SAFETY: a cancelled pending callback owned exactly one Arc
            // transferred by `enqueue_delayed`.
            drop(unsafe { Arc::from_raw(Arc::as_ptr(self)) });
        }
    }
}

impl WorkItem for State {
    type Pointer = Arc<State>;

    fn run(this: Arc<State>) {
        if !this.active.load(Acquire) {
            return;
        }
        this.poll();
        if this.active.load(Acquire) {
            this.queue_poll();
        }
    }
}

impl WorkItem<IDLE_WORK> for State {
    type Pointer = Arc<State>;

    fn run(this: Arc<State>) {
        let mut s = this.stream.lock();
        let Some(deadline) = s.direct_until else {
            return;
        };
        let remaining_ms = (deadline - Instant::<Monotonic>::now()).as_millis_ceil();
        match s.check_idle(remaining_ms) {
            IdleStep::Ignore => {}
            IdleStep::RearmMs(delay_ms) => this.schedule_idle(delay_ms),
            IdleStep::Expire => {
                if let Err(e) = this.sync_locked(&mut s) {
                    dev_warn!(this.control.dev, "client not closed after idle: {:?}\n", e);
                }
            }
        }
    }
}

#[pinned_drop]
impl PinnedDrop for State {
    fn drop(self: Pin<&mut Self>) {
        if self.retain_ring.load(Acquire) {
            if let Some(ring) = self.ring.lock().take() {
                // The outcome of ClientOpen/ClientClose is unknown, so the
                // firmware may retain this IOVA until the AOP reboots.
                mem::forget(ring);
            }
        }
    }
}

static SCAN_MASKS: [usize; 2] = [1, 0];

/// # Safety
///
/// The IIO core supplies the registered device, channel, and output arrays.
unsafe extern "C" fn read_raw_multi(
    indio_dev: *mut bindings::iio_dev,
    _chan: *const bindings::iio_chan_spec,
    max_len: i32,
    vals: *mut i32,
    val_len: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state: Arc<State> = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) }.into();
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ as isize {
        // SAFETY: The caller provided at least one element.
        unsafe {
            *vals = state.hz.load(Relaxed) as i32;
            *val_len = 1;
        }
        return bindings::IIO_VAL_INT as i32;
    }
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_SCALE as isize {
        if max_len < 2 {
            return EINVAL.to_errno();
        }
        // SAFETY: The caller provided at least two elements.
        unsafe {
            *vals = 1;
            *vals.add(1) = Q_FRAC_BITS;
            *val_len = 2;
        }
        return bindings::IIO_VAL_FRACTIONAL_LOG2 as i32;
    }
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_RAW as isize || max_len < 4 {
        return EINVAL.to_errno();
    }
    let quat = match state.read_now() {
        Ok(quat) => quat,
        Err(e) => return e.to_errno(),
    };
    // SAFETY: The caller provided at least four elements.
    unsafe {
        ptr::copy_nonoverlapping(quat.as_ptr(), vals, quat.len());
        *val_len = quat.len() as i32;
    }
    bindings::IIO_VAL_INT_MULTIPLE as i32
}

/// # Safety
///
/// The IIO core supplies the registered device.
unsafe extern "C" fn write_raw(
    indio_dev: *mut bindings::iio_dev,
    _chan: *const bindings::iio_chan_spec,
    val: c_int,
    val2: c_int,
    mask: isize,
) -> c_int {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ as isize
        || val2 != 0
        || !(1..=MAX_HZ as c_int).contains(&val)
    {
        return EINVAL.to_errno();
    }
    // The next poll and the next client use the new rate.
    state.hz.store(val as u32, Relaxed);
    0
}

/// # Safety
///
/// The IIO core supplies the registered device.
unsafe extern "C" fn buffer_postenable(indio_dev: *mut bindings::iio_dev) -> c_int {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state: Arc<State> = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) }.into();
    match state.set_buffer(true) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
///
/// The IIO core supplies the registered device.
unsafe extern "C" fn buffer_predisable(indio_dev: *mut bindings::iio_dev) -> c_int {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state: Arc<State> = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) }.into();
    match state.set_buffer(false) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

static BUFFER_OPS: bindings::iio_buffer_setup_ops = bindings::iio_buffer_setup_ops {
    preenable: None,
    postenable: Some(buffer_postenable),
    predisable: Some(buffer_predisable),
    postdisable: None,
    validate_scan_mask: None,
};

fn quaternion_channel() -> bindings::iio_chan_spec {
    let mut chan = bindings::iio_chan_spec {
        type_: bindings::BINDINGS_IIO_ROT,
        channel2: bindings::BINDINGS_IIO_MOD_QUATERNION as i32,
        scan_index: 0,
        __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
            scan_type: bindings::iio_scan_type {
                __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 { sign: b's' },
                realbits: 32,
                storagebits: 32,
                repeat: 4,
                ..Default::default()
            },
        },
        info_mask_separate: (1usize << bindings::BINDINGS_IIO_CHAN_INFO_RAW)
            | (1usize << bindings::BINDINGS_IIO_CHAN_INFO_SCALE),
        info_mask_shared_by_all: 1usize << bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ,
        ..Default::default()
    };
    chan.set_modified(1);
    chan
}

fn timestamp_channel() -> bindings::iio_chan_spec {
    bindings::iio_chan_spec {
        type_: bindings::BINDINGS_IIO_TIMESTAMP,
        channel: -1,
        scan_index: 1,
        __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
            scan_type: bindings::iio_scan_type {
                __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 { sign: b's' },
                realbits: 64,
                storagebits: 64,
                ..Default::default()
            },
        },
        ..Default::default()
    }
}

struct Spec {
    channels: [bindings::iio_chan_spec; 2],
    info: bindings::iio_info,
    _p: PhantomPinned,
}

struct MotionIio {
    dev: *mut bindings::iio_dev,
    _spec: Pin<KBox<Spec>>,
    registered: bool,
}

impl MotionIio {
    fn new(pdev: &platform::Device<Core<'_>>, control: Arc<Control>) -> Result<(Self, Arc<State>)> {
        let dev = pdev.as_ref();
        let payload = control.get(PROP_FASTPATH_PAYLOAD_SIZE, 8)?;
        // The reply is a 32-bit value on the M4 Pro; accept 64 bits as well.
        let size = match payload.len() {
            8.. => le_u64(&payload, 0),
            4..=7 => le_u32(&payload, 0).map(u64::from),
            _ => None,
        };
        let Some(size) = size else {
            dev_err!(
                dev,
                "fast-path payload size reply is {} bytes\n",
                payload.len()
            );
            return Err(EIO);
        };
        let payload_len = usize::try_from(size).map_err(|_| EINVAL)?;
        if !(5..=MAX_REPORT_LEN).contains(&payload_len) {
            dev_err!(dev, "invalid fast-path payload size {}\n", payload_len);
            return Err(EINVAL);
        }
        let stride = (payload_len + 75) & !63;
        let required = FASTPATH_HEADER_LEN + 2 * stride;
        let ring_size = (required + FASTPATH_PAGE_SIZE - 1) & !(FASTPATH_PAGE_SIZE - 1);
        // The AOP firmware writes the ring, so it is mapped through the AOP
        // core's own DART stream: the service device has no DMA setup.
        let aop_dev = dev.parent().ok_or(ENODEV)?;
        // SAFETY: The AOP core unbinds its service devices before it unbinds
        // itself, so the parent is bound while this probe runs. The reference
        // is used only for the allocation; the buffer keeps its own device
        // reference.
        let aop_dev = unsafe { aop_dev.as_bound() };
        let ring = Coherent::<u8>::zeroed_slice(aop_dev, ring_size, GFP_KERNEL)?;
        let spec = KBox::pin(
            Spec {
                channels: [quaternion_channel(), timestamp_channel()],
                info: bindings::iio_info {
                    read_raw_multi: Some(read_raw_multi),
                    write_raw: Some(write_raw),
                    ..Default::default()
                },
                _p: PhantomPinned,
            },
            GFP_KERNEL,
        )?;
        // SAFETY: `dev` is a valid platform device.
        let indio_dev = unsafe { bindings::iio_device_alloc(dev.as_raw(), 0) };
        if indio_dev.is_null() {
            return Err(ENOMEM);
        }
        let state = match Arc::pin_init(
            pin_init!(State {
                iio: IioPtr(indio_dev),
                control,
                payload_len,
                stride,
                ring <- new_mutex!(Some(ring)),
                sample <- new_spinlock!(Sample::default()),
                active: Atomic::new(false),
                opened: Atomic::new(false),
                retain_ring: Atomic::new(false),
                client_id: Atomic::new(0),
                next_client_id: Atomic::new(1),
                last_aop_timestamp: Atomic::new(0),
                hz: Atomic::new(DEFAULT_HZ),
                stream <- new_mutex!(Stream::new()),
                poll_work <- new_delayed_work!("State::poll_work"),
                idle_work <- new_delayed_work!("State::idle_work"),
            }),
            GFP_KERNEL,
        ) {
            Ok(state) => state,
            Err(e) => {
                // SAFETY: Allocated above and not registered.
                unsafe { bindings::iio_device_free(indio_dev) };
                return Err(e);
            }
        };
        // SAFETY: The IIO device is unregistered and `spec` is pinned.
        unsafe {
            (*indio_dev).priv_ = state.clone().into_foreign().cast();
            (*indio_dev).name = c"aop-sensors-devmotion6".as_char_ptr();
            (*indio_dev).channels = spec.channels.as_ptr();
            (*indio_dev).num_channels = spec.channels.len() as i32;
            (*indio_dev).info = &spec.info;
            (*indio_dev).modes = bindings::INDIO_DIRECT_MODE as i32;
            (*indio_dev).available_scan_masks = SCAN_MASKS.as_ptr();
        }
        // SAFETY: Both devices are valid and the buffer is device-managed.
        let ret = unsafe {
            bindings::devm_iio_kfifo_buffer_setup_ext(
                dev.as_raw(),
                indio_dev,
                &BUFFER_OPS,
                ptr::null_mut(),
            )
        };
        if ret < 0 {
            // SAFETY: The device is not registered; the private pointer is
            // the foreign `Arc` installed above.
            unsafe {
                drop(Arc::<State>::from_foreign((*indio_dev).priv_.cast()));
                bindings::iio_device_free(indio_dev);
            }
            return Err(Error::from_errno(ret));
        }
        Ok((
            Self {
                dev: indio_dev,
                _spec: spec,
                registered: false,
            },
            state,
        ))
    }

    fn register(&mut self) -> Result {
        let module = kernel::module::this_module::<crate::LocalModule>();
        // SAFETY: The device is fully initialized and unregistered.
        let ret = unsafe { bindings::__iio_device_register(self.dev, module.as_ptr()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        self.registered = true;
        Ok(())
    }
}

impl Drop for MotionIio {
    fn drop(&mut self) {
        // SAFETY: The device and its private `Arc` were installed by `new()`.
        unsafe {
            if self.registered {
                bindings::iio_device_unregister(self.dev);
            }
            drop(Arc::<State>::from_foreign((*self.dev).priv_.cast()));
            bindings::iio_device_free(self.dev);
        }
    }
}

// SAFETY: The IIO core serializes registration and removal.
unsafe impl Send for MotionIio {}
// SAFETY: As above; there are no methods taking `&self`.
unsafe impl Sync for MotionIio {}

#[repr(C)]
struct PmHook {
    nb: Opaque<bindings::notifier_block>,
    state: Arc<State>,
    registered: Atomic<bool>,
}

/// # Safety
///
/// `nb` is the first field of a registered `PmHook`.
unsafe extern "C" fn pm_event(
    nb: *mut bindings::notifier_block,
    action: usize,
    _data: *mut c_void,
) -> c_int {
    // SAFETY: By the callback contract above.
    let hook = unsafe { &*nb.cast::<PmHook>() };
    let ret = match action as u32 {
        bindings::PM_SUSPEND_PREPARE
        | bindings::PM_HIBERNATION_PREPARE
        | bindings::PM_RESTORE_PREPARE => hook.state.set_parked(true),
        bindings::PM_POST_SUSPEND | bindings::PM_POST_HIBERNATION | bindings::PM_POST_RESTORE => {
            hook.state.set_parked(false)
        }
        _ => return bindings::NOTIFY_DONE as c_int,
    };
    if let Err(e) = ret {
        dev_warn!(
            hook.state.control.dev,
            "reports not switched for sleep: {:?}\n",
            e
        );
    }
    bindings::NOTIFY_OK as c_int
}

#[cfg(CONFIG_PM_SLEEP)]
/// # Safety
///
/// `nb` remains pinned until [`pm_unregister`] is called.
unsafe fn pm_register(nb: *mut bindings::notifier_block) -> c_int {
    // SAFETY: The caller keeps the block pinned through unregister.
    unsafe { bindings::register_pm_notifier(nb) }
}

#[cfg(CONFIG_PM_SLEEP)]
/// # Safety
///
/// `nb` was registered by [`pm_register`].
unsafe fn pm_unregister(nb: *mut bindings::notifier_block) {
    // SAFETY: The block was registered by `pm_register`.
    unsafe { bindings::unregister_pm_notifier(nb) };
}

#[cfg(not(CONFIG_PM_SLEEP))]
/// # Safety
///
/// No requirements; this configuration has no PM notifier.
unsafe fn pm_register(_nb: *mut bindings::notifier_block) -> c_int {
    0
}

#[cfg(not(CONFIG_PM_SLEEP))]
/// # Safety
///
/// No requirements; this configuration has no PM notifier.
unsafe fn pm_unregister(_nb: *mut bindings::notifier_block) {}

// SAFETY: The PM core serializes notifier operations.
unsafe impl Send for PmHook {}
// SAFETY: As above.
unsafe impl Sync for PmHook {}

impl PmHook {
    fn new(state: Arc<State>) -> Result<Pin<KBox<Self>>> {
        let hook = KBox::pin(
            Self {
                nb: Opaque::new(bindings::notifier_block {
                    notifier_call: Some(pm_event),
                    next: ptr::null_mut(),
                    priority: 0,
                }),
                state,
                registered: Atomic::new(false),
            },
            GFP_KERNEL,
        )?;
        // SAFETY: The notifier remains pinned in `hook`.
        let ret = unsafe { pm_register(hook.nb.get()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        hook.registered.store(true, Relaxed);
        Ok(hook)
    }

    fn unregister(&self) {
        if self.registered.xchg(false, Relaxed) {
            // SAFETY: Registered by `new()` and still pinned.
            unsafe { pm_unregister(self.nb.get()) };
        }
    }
}

impl Drop for PmHook {
    fn drop(&mut self) {
        self.unregister();
    }
}

struct IIOAopDevMotionDriver {
    pm: Pin<KBox<PmHook>>,
    state: Arc<State>,
    _iio: MotionIio,
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-devmotion6")), ())]
);

impl platform::Driver for IIOAopDevMotionDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopDevMotionDriver;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound ()>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let dev = pdev.as_ref();
        // SAFETY: This binds only to children created by the AOP core.
        let aop = unsafe { <dyn AOP>::from_child(dev) }?;
        // SAFETY: The AOP core attaches an `EPICService` as platform data.
        let service = unsafe { (*dev.as_raw()).platform_data.cast::<EPICService>() };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: The record lives as long as this child device.
        let service = unsafe { *service };
        let control = Arc::new(
            Control {
                dev: dev.into(),
                aop: aop.clone(),
                service,
            },
            GFP_KERNEL,
        )?;
        control.calibrate()?;
        let (mut iio, state) = MotionIio::new(pdev, control)?;
        iio.register()?;
        let pm = PmHook::new(state.clone())?;
        Ok(IIOAopDevMotionDriver {
            pm,
            state,
            _iio: iio,
        })
    }

    fn unbind(_dev: &platform::Device<Core<'_>>, this: Pin<&Self>) {
        this.pm.unregister();
        let _ = this.state.shutdown();
        this.state.disable_poll();
    }
}

/// The AOP core creates the device of a service, named after the service,
/// whenever the firmware announces that service. Registering the driver under
/// the same name lets the platform bus bind it on every machine whose AOP
/// offers the service, with or without a device tree node for it.
const SERVICE_NAME: &CStr = c_str!("devmotion6");

#[pin_data]
struct IIOAopDevMotionModule {
    #[pin]
    _driver: driver::Registration<platform::Adapter<IIOAopDevMotionDriver>>,
}

impl kernel::InPlaceModule for IIOAopDevMotionModule {
    fn init(module: &'static ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _driver <- driver::Registration::new(SERVICE_NAME, module),
        })
    }
}

module! {
    type: IIOAopDevMotionModule,
    name: "iio_aop_devmotion",
    description: "AOP device-motion fusion driver",
    license: "Dual MIT/GPL",
    alias: ["platform:devmotion6"],
}

#[kunit_tests(iio_aop_devmotion)]
mod tests {
    use super::*;
    use core::mem::MaybeUninit;

    /// The bytes the IIO core takes for a scan of `channels`, all enabled:
    /// each element at a multiple of its size, a repeated one counted to the
    /// next power of two, and the total a multiple of the largest, as
    /// `iio_compute_scan_bytes()` lays them out.
    fn scan_bytes(channels: &[bindings::iio_chan_spec]) -> usize {
        let mut bytes = 0usize;
        let mut largest = 1;
        for chan in channels {
            // SAFETY: The channels of this driver use the plain scan type.
            let scan_type = unsafe { chan.__bindgen_anon_1.scan_type };
            let repeat = usize::from(scan_type.repeat.max(1)).next_power_of_two();
            let len = usize::from(scan_type.storagebits) / 8 * repeat;
            bytes = bytes.next_multiple_of(len) + len;
            largest = largest.max(len);
        }
        bytes.next_multiple_of(largest)
    }

    #[test]
    fn scan_is_written_in_full() {
        let size = scan_bytes(&[quaternion_channel(), timestamp_channel()]);
        assert_eq!(mem::size_of::<Scan>(), size);

        let written = mem::size_of::<[i32; 4]>() + mem::size_of::<i64>();
        let mut slot = MaybeUninit::<Scan>::uninit();
        // SAFETY: `slot` is valid for a write of one `Scan`, first of a byte
        // pattern and then of a scan built as `State::publish()` builds it.
        unsafe {
            ptr::write_bytes(slot.as_mut_ptr(), 0xa5, 1);
            slot.as_mut_ptr().write(Scan::new([1, 2, 3, 4], 5));
        }
        for at in written..size {
            // SAFETY: `at` is within `slot`, all of which was written above.
            let byte = unsafe { slot.as_ptr().cast::<u8>().add(at).read_volatile() };
            assert_eq!(byte, 0);
        }
    }
}
