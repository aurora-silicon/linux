// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP temperature sensor driver
//!
//! The `als-temp` service on current laptops is a board-temperature sensor.
//! Its fake-HID report carries a signed Q16.16 Celsius value and an AOP
//! timestamp. Calibration data, when supplied by the bootloader, is sent
//! once per AOP boot before reports are enabled.
//!
//! When the service accepts clearing its report interval, reports run only
//! while someone reads them: while a buffer is enabled, or until direct reads
//! go quiet for a short idle window. With nobody reading, the reports stay off
//! so the AOP does not wake the application processor once a second at idle.
//!
//! The AOP core creates a device for the service whenever the firmware
//! announces it, and the driver binds to that device by name. A device tree
//! node for the service is optional; it only carries the calibration.

use core::{
    ffi::{c_int, c_void},
    marker::PhantomPinned,
    ptr,
};

use kernel::{
    bindings, c_str,
    device::{self, Bound, Core},
    devres, driver, new_delayed_work, new_mutex, new_spinlock, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, FakehidListener, FakehidRegistration, AOP},
    sync::{
        aref::ARef,
        atomic::{Atomic, Relaxed},
        Arc, Mutex, SpinLock,
    },
    time::{self, delay::fsleep, Delta, Instant, Monotonic},
    types::{ForeignOwnable, Opaque},
    workqueue::{self, impl_has_delayed_work, DelayedWork, Work, WorkItem},
    ThisModule,
};

const SUBTYPE_SET_PROPERTY: u16 = 4;
const PROP_INTERVAL: u32 = 0;
const PROP_CALIBRATION_STATE: u32 = 61;
const INTERVAL_US: u32 = 1_000_000;
/// How long a direct read waits for the first 1 Hz report of a fresh start.
const FIRST_SAMPLE_MS: i64 = 1_500;
const FIRST_SAMPLE_POLL_MS: i64 = 20;
/// How long reports stay on after the last direct read without a buffer.
const DIRECT_IDLE_MS: i64 = 5_000;
const STATUS_UNSUPPORTED: u32 = 0xe000_02c7;
const REPORT_LEN: usize = 14;
const TEMP_OFFSET: usize = 2;

const CALIBRATION: [(u32, &CStr); 4] = [
    (11, c_str!("apple,temp-offset-calibration")),
    (208, c_str!("apple,absolute-temp-offset-cal")),
    (208, c_str!("apple,absolute-temp-offset-fcm-cal")),
    (208, c_str!("apple,absolute-temp-offset-bcm-cal")),
];

fn le_u32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn location0_packet(prop: &[u8]) -> Option<&[u8]> {
    if prop.len() < 4 || prop[3] != 1 {
        return None;
    }
    let mut at = 4;
    for _ in 0..prop[2] {
        let location = le_u32(prop, at)?;
        let len = usize::try_from(le_u32(prop, at + 4)?).ok()?;
        let start = at.checked_add(8)?;
        let packet = prop.get(start..start.checked_add(len)?)?;
        if location == 0 {
            return Some(packet);
        }
        at = start + len;
    }
    None
}

fn decode_millicelsius(report: &[u8]) -> Option<i32> {
    if report.len() != REPORT_LEN {
        return None;
    }
    let raw = le_u32(report, TEMP_OFFSET)? as i32;
    Some(((i64::from(raw) * 1000) / 65_536) as i32)
}

/// What a stream state change requires from the service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SyncAction {
    None,
    /// Enable reports; send calibration packets first when `calibrate` is true.
    Start {
        calibrate: bool,
    },
    Stop,
}

/// Outcome of checking the direct-read idle deadline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum IdleStep {
    /// No direct-read hold to expire.
    Ignore,
    /// More reads extended the deadline; re-arm the work for this many ms.
    RearmMs(u32),
    /// The deadline passed; stop the reports if nothing else wants them.
    Expire,
}

/// Observed firmware reporting state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RunState {
    Stopped,
    Running,
    /// A start or stop write was not acknowledged; the next transition
    /// reasserts the desired interval in either direction.
    Unknown,
}

/// Who wants the reports of the service and whether calibration has already
/// been sent in this AOP boot.
struct Stream<T> {
    /// Calibration has been sent in this AOP boot.
    calibrated: bool,
    /// Service accepted `PROP_INTERVAL = 0` at probe so reports can idle-stop.
    /// When false, reports stay running continuously except across sleep/unbind.
    on_demand: bool,
    /// A buffer is enabled.
    buffer_on: bool,
    /// Number of direct reads currently waiting for a sample.
    direct_readers: u32,
    /// Reports are held on for recent direct reads until this deadline.
    direct_until: Option<T>,
    /// A start for a direct read failed with this error; direct reads return
    /// it without another try until this deadline.
    failed_until: Option<(T, Error)>,
    /// Set across system sleep and after unbind: no reports.
    parked: bool,
    /// Current knowledge of whether firmware reports are running.
    run_state: RunState,
}

impl<T> Stream<T> {
    const fn new() -> Self {
        Self {
            calibrated: false,
            on_demand: false,
            buffer_on: false,
            direct_readers: 0,
            direct_until: None,
            failed_until: None,
            parked: false,
            run_state: RunState::Stopped,
        }
    }

    fn want(&self) -> bool {
        if self.parked {
            return false;
        }
        !self.on_demand || self.buffer_on || self.direct_readers > 0 || self.direct_until.is_some()
    }

    fn next_action(&self) -> SyncAction {
        match (self.want(), self.run_state) {
            (true, RunState::Stopped | RunState::Unknown) => SyncAction::Start {
                calibrate: !self.calibrated,
            },
            (false, RunState::Running | RunState::Unknown) => SyncAction::Stop,
            _ => SyncAction::None,
        }
    }

    /// Evaluates the direct-read idle deadline given the milliseconds left
    /// until it expires (`<= 0` once expired).
    fn check_idle(&mut self, remaining_ms: i64) -> IdleStep {
        if self.direct_until.is_none() {
            return IdleStep::Ignore;
        }
        if !self.on_demand || self.parked || self.buffer_on {
            self.direct_until = None;
            return IdleStep::Ignore;
        }
        if self.direct_readers > 0 {
            return IdleStep::RearmMs(DIRECT_IDLE_MS as u32);
        }
        if remaining_ms > 0 {
            IdleStep::RearmMs(remaining_ms.clamp(1, DIRECT_IDLE_MS) as u32)
        } else {
            self.direct_until = None;
            IdleStep::Expire
        }
    }

    /// Returns the error of a failed start for as long as direct reads back
    /// off from it, given the milliseconds left until they may try again
    /// (`<= 0` once they may).
    fn check_backoff(&mut self, remaining_ms: i64) -> Option<Error> {
        if remaining_ms <= 0 {
            self.failed_until = None;
        }
        self.failed_until.as_ref().map(|(_, e)| *e)
    }
}

#[pin_data]
struct Control {
    dev: ARef<device::Device>,
    aop: Arc<dyn AOP>,
    service: EPICService,
    packets: KVec<(u32, KVec<u8>)>,
    /// Serializes starting and stopping the reports.
    #[pin]
    stream: Mutex<Stream<Instant<Monotonic>>>,
    /// Stops direct-read reports once reads go quiet.
    #[pin]
    idle_work: DelayedWork<Control>,
}

impl_has_delayed_work! {
    impl HasDelayedWork<Self> for Control { self.idle_work }
}

impl Control {
    fn new(dev: &device::Device, aop: Arc<dyn AOP>, service: EPICService) -> Result<Arc<Self>> {
        let mut packets = KVec::new();
        // Without a device tree node for the service there is no calibration
        // to send; the sensor then runs on its defaults.
        if let Some(node) = dev.fwnode() {
            for (prop, name) in CALIBRATION {
                if !node.property_present(name) {
                    continue;
                }
                let len = node.property_count_elem::<u8>(name)?;
                let data = node
                    .property_read_array_vec::<u8>(name, len)?
                    .required_by(dev)?;
                let Some(packet) = location0_packet(&data) else {
                    dev_err!(dev, "{:?} holds no calibration for location 0\n", name);
                    return Err(EINVAL);
                };
                let mut owned = KVec::new();
                owned.extend_from_slice(packet, GFP_KERNEL)?;
                packets.push((prop, owned), GFP_KERNEL)?;
            }
        }
        if packets.is_empty() {
            dev_dbg!(
                dev,
                "no temperature calibration from the bootloader; using sensor defaults\n"
            );
        }
        Arc::pin_init(
            pin_init!(Self {
                dev: dev.into(),
                aop,
                service,
                packets,
                stream <- new_mutex!(Stream::new()),
                idle_work <- new_delayed_work!("Control::idle_work"),
            }),
            GFP_KERNEL,
        )
    }

    fn write(&self, prop: u32, value: &[u8]) -> Result<u32> {
        let mut msg = KVec::with_capacity(8 + value.len(), GFP_KERNEL)?;
        msg.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(&prop.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(value, GFP_KERNEL)?;
        self.aop
            .epic_call(&self.service, SUBTYPE_SET_PROPERTY, &msg)
    }

    fn calibrate(&self) -> Result {
        for (prop, packet) in &self.packets {
            let status = self.write(*prop, packet)?;
            if status != 0 && status != STATUS_UNSUPPORTED {
                dev_err!(
                    self.dev,
                    "calibration property {} refused: status {:#x}\n",
                    prop,
                    status
                );
                return Err(EIO);
            }
        }
        let calibration_state = if self.packets.is_empty() { 1u32 } else { 2u32 };
        let status = self.write(PROP_CALIBRATION_STATE, &calibration_state.to_le_bytes())?;
        if status != 0 {
            dev_err!(
                self.dev,
                "calibration state refused: status {:#x}\n",
                status
            );
            return Err(EIO);
        }
        Ok(())
    }

    fn start_reports(&self) -> Result {
        let status = self.write(PROP_INTERVAL, &INTERVAL_US.to_le_bytes())?;
        if status != 0 {
            dev_err!(self.dev, "interval refused: status {:#x}\n", status);
            return Err(EIO);
        }
        Ok(())
    }

    fn stop_reports(&self) -> Result {
        let status = self.write(PROP_INTERVAL, &0u32.to_le_bytes())?;
        if status != 0 {
            dev_err!(self.dev, "stop refused: status {:#x}\n", status);
            return Err(EIO);
        }
        Ok(())
    }

    /// Checks once at probe that the service accepts calibration and the
    /// report interval, then stops reports for on-demand operation. If the
    /// firmware explicitly refuses `PROP_INTERVAL = 0`, retain continuous
    /// mode rather than dropping a working sensor.
    fn probe_init(&self) -> Result {
        let mut s = self.stream.lock();
        self.calibrate()?;
        s.calibrated = true;
        match self.write(PROP_INTERVAL, &INTERVAL_US.to_le_bytes()) {
            Ok(0) => {
                s.run_state = RunState::Running;
            }
            Ok(status) => {
                dev_err!(self.dev, "interval refused: status {:#x}\n", status);
                return Err(EIO);
            }
            Err(e) => {
                // An unacknowledged start may still have taken effect in
                // firmware; mark unknown so shutdown and best-effort rollback
                // attempt to stop it.
                s.run_state = RunState::Unknown;
                if let Ok(0) = self.write(PROP_INTERVAL, &0u32.to_le_bytes()) {
                    s.run_state = RunState::Stopped;
                }
                return Err(e);
            }
        }
        match self.write(PROP_INTERVAL, &0u32.to_le_bytes()) {
            Ok(0) => {
                s.run_state = RunState::Stopped;
                s.on_demand = true;
                Ok(())
            }
            Ok(status) => {
                dev_info!(
                    self.dev,
                    "idle stop refused (status {:#x}); keeping continuous reports\n",
                    status
                );
                s.on_demand = false;
                Ok(())
            }
            Err(e) => {
                // Transient transport error after start: attempt best-effort stop.
                s.run_state = RunState::Unknown;
                if let Ok(0) = self.write(PROP_INTERVAL, &0u32.to_le_bytes()) {
                    s.run_state = RunState::Stopped;
                }
                Err(e)
            }
        }
    }

    /// Starts or stops the reports to match their users. The caller holds the
    /// stream lock. `before_start` runs right before enabling reports so any
    /// cached sample from an earlier run is invalidated before new reports
    /// arrive.
    fn sync_locked(
        &self,
        s: &mut Stream<Instant<Monotonic>>,
        before_start: impl FnOnce(),
    ) -> Result {
        match s.next_action() {
            SyncAction::Start { calibrate } => {
                if calibrate {
                    self.calibrate()?;
                    s.calibrated = true;
                }
                before_start();
                match self.start_reports() {
                    Ok(()) => {
                        s.run_state = RunState::Running;
                        Ok(())
                    }
                    Err(e) => {
                        s.run_state = RunState::Unknown;
                        Err(e)
                    }
                }
            }
            SyncAction::Stop => match self.stop_reports() {
                Ok(()) => {
                    s.run_state = RunState::Stopped;
                    Ok(())
                }
                Err(e) => {
                    s.run_state = RunState::Unknown;
                    Err(e)
                }
            },
            SyncAction::None => Ok(()),
        }
    }

    /// Arms a rate-limited idle-stop retry whenever an unacknowledged start or
    /// stop leaves an idle on-demand stream potentially running.
    fn arm_stop_retry_locked(&self, s: &mut Stream<Instant<Monotonic>>) -> bool {
        if s.on_demand && !s.want() && s.run_state != RunState::Stopped {
            s.direct_until = Some(Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS));
            true
        } else {
            false
        }
    }

    fn schedule_idle(self: &Arc<Self>, delay_ms: u32) {
        let _ =
            workqueue::system_dfl().enqueue_delayed(self.clone(), time::msecs_to_jiffies(delay_ms));
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

    fn set_buffer(self: &Arc<Self>, on: bool, before_start: impl FnOnce()) -> Result {
        let (ret, retry) = {
            let mut s = self.stream.lock();
            s.buffer_on = on;
            if on {
                s.direct_until = None;
                s.failed_until = None;
            }
            let ret = self.sync_locked(&mut s, before_start);
            if on && ret.is_err() {
                // The IIO core does not call predisable after a failed enable.
                s.buffer_on = false;
            }
            let retry = ret.is_err() && self.arm_stop_retry_locked(&mut s);
            (ret, retry)
        };
        if retry {
            self.schedule_idle(DIRECT_IDLE_MS as u32);
        }
        ret
    }

    fn set_parked(
        self: &Arc<Self>,
        parked: bool,
        reset_cal: bool,
        before_start: impl FnOnce(),
    ) -> Result {
        let (ret, retry) = {
            let mut s = self.stream.lock();
            s.parked = parked;
            s.failed_until = None;
            if parked {
                s.direct_until = None;
            } else if reset_cal {
                s.calibrated = false;
                s.run_state = RunState::Stopped;
            }
            let ret = self.sync_locked(&mut s, before_start);
            let retry = !parked && ret.is_err() && self.arm_stop_retry_locked(&mut s);
            (ret, retry)
        };
        if parked {
            self.stop_idle_sync(false);
        } else if retry {
            self.schedule_idle(DIRECT_IDLE_MS as u32);
        }
        ret
    }

    fn shutdown(self: &Arc<Self>) -> Result {
        let ret = {
            let mut s = self.stream.lock();
            s.parked = true;
            s.direct_until = None;
            s.failed_until = None;
            self.sync_locked(&mut s, || {})
        };
        self.stop_idle_sync(true);
        ret
    }
}

impl WorkItem for Control {
    type Pointer = Arc<Control>;

    fn run(this: Arc<Control>) {
        let retry = {
            let mut s = this.stream.lock();
            let Some(deadline) = s.direct_until else {
                return;
            };
            let remaining_ms = (deadline - Instant::<Monotonic>::now()).as_millis_ceil();
            match s.check_idle(remaining_ms) {
                IdleStep::Ignore => None,
                IdleStep::RearmMs(delay_ms) => Some(delay_ms),
                IdleStep::Expire => match this.sync_locked(&mut s, || {}) {
                    Ok(()) => None,
                    Err(e) => {
                        dev_warn!(this.dev, "reports not stopped after idle: {:?}\n", e);
                        if this.arm_stop_retry_locked(&mut s) {
                            Some(DIRECT_IDLE_MS as u32)
                        } else {
                            None
                        }
                    }
                },
            }
        };
        if let Some(delay_ms) = retry {
            this.schedule_idle(delay_ms);
        }
    }
}

#[derive(Clone, Copy, Default)]
struct Sample {
    millicelsius: i32,
    valid: bool,
}

#[repr(C, align(8))]
struct Scan {
    millicelsius: i32,
    pad: u32,
    timestamp: i64,
}

#[pin_data]
struct State {
    iio: IioPtr,
    control: Arc<Control>,
    #[pin]
    sample: SpinLock<Sample>,
    /// Gates `iio_push_to_buffers()` and synchronously drains in-flight pushes
    /// during `buffer_predisable` before the IIO core detaches the buffer list.
    #[pin]
    push_enabled: SpinLock<bool>,
}

/// Keeps `direct_readers` accounted for across `read_now()`, and arms the
/// idle-stop deadline once the reader finishes.
struct DirectReadGuard<'a>(&'a Arc<Control>);

impl Drop for DirectReadGuard<'_> {
    fn drop(&mut self) {
        let schedule = {
            let mut s = self.0.stream.lock();
            s.direct_readers = s.direct_readers.saturating_sub(1);
            if s.on_demand && !s.parked && !s.buffer_on {
                s.direct_until =
                    Some(Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS));
                true
            } else {
                false
            }
        };
        if schedule {
            self.0.schedule_idle(DIRECT_IDLE_MS as u32);
        }
    }
}

impl State {
    /// Returns the latest temperature sample. Without a buffer the reports are
    /// started on demand and kept running until direct reads go quiet for
    /// `DIRECT_IDLE_MS`.
    fn read_now(&self) -> Result<i32> {
        let guard = {
            let mut s = self.control.stream.lock();
            if !s.on_demand {
                drop(s);
                let sample = *self.sample.lock();
                return if sample.valid {
                    Ok(sample.millicelsius)
                } else {
                    Err(ENODATA)
                };
            }
            if s.parked {
                return Err(EBUSY);
            }
            if let Some((until, _)) = s.failed_until {
                let remaining_ms = (until - Instant::<Monotonic>::now()).as_millis_ceil();
                if let Some(e) = s.check_backoff(remaining_ms) {
                    return Err(e);
                }
            }
            s.direct_readers += 1;
            if let Err(e) = self
                .control
                .sync_locked(&mut s, || self.sample.lock().valid = false)
            {
                s.direct_readers = s.direct_readers.saturating_sub(1);
                let until = Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
                s.failed_until = Some((until, e));
                let retry = self.control.arm_stop_retry_locked(&mut s);
                drop(s);
                if retry {
                    self.control.schedule_idle(DIRECT_IDLE_MS as u32);
                }
                return Err(e);
            }
            DirectReadGuard(&self.control)
        };

        let mut waited = 0;
        while waited < FIRST_SAMPLE_MS && !self.sample.lock().valid {
            if self.control.stream.lock().parked {
                return Err(EBUSY);
            }
            fsleep(Delta::from_millis(FIRST_SAMPLE_POLL_MS));
            waited += FIRST_SAMPLE_POLL_MS;
        }
        let sample = *self.sample.lock();
        drop(guard);
        if sample.valid {
            Ok(sample.millicelsius)
        } else {
            Err(ENODATA)
        }
    }
}

struct IioPtr(*mut bindings::iio_dev);

// SAFETY: The IIO device may be used from any thread.
unsafe impl Send for IioPtr {}
// SAFETY: As above.
unsafe impl Sync for IioPtr {}

impl FakehidListener for State {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()> {
        let millicelsius = decode_millicelsius(data).ok_or(EMSGSIZE)?;
        *self.sample.lock() = Sample {
            millicelsius,
            valid: true,
        };
        let push = self.push_enabled.lock();
        if *push {
            // SAFETY: The buffer is enabled and `buffer_predisable` clears
            // `push_enabled` under this lock before the IIO core removes the
            // buffer from the active list.
            let timestamp = unsafe { bindings::iio_get_time_ns(self.iio.0) };
            let scan = Scan {
                millicelsius,
                pad: 0,
                timestamp,
            };
            // SAFETY: The scan layout matches the only available scan mask.
            unsafe { bindings::iio_push_to_buffers(self.iio.0, ptr::from_ref(&scan).cast()) };
        }
        Ok(())
    }
}

static SCAN_MASKS: [usize; 2] = [1, 0];

/// # Safety
///
/// The IIO core supplies the registered device and output pointers.
unsafe extern "C" fn read_raw(
    indio_dev: *mut bindings::iio_dev,
    _chan: *const bindings::iio_chan_spec,
    val: *mut i32,
    val2: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ as isize {
        // SAFETY: The pointers are the caller's result variables.
        unsafe {
            *val = (1_000_000 / INTERVAL_US) as i32;
            *val2 = 0;
        }
        return bindings::IIO_VAL_INT as i32;
    }
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED as isize {
        return EINVAL.to_errno();
    }
    let millicelsius = match state.read_now() {
        Ok(value) => value,
        Err(e) => return e.to_errno(),
    };
    // SAFETY: The pointers are the caller's result variables. IIO temperature
    // processed values are in millidegrees Celsius.
    unsafe {
        *val = millicelsius;
        *val2 = 0;
    }
    bindings::IIO_VAL_INT as i32
}

/// Starts the reports for an enabled buffer.
///
/// # Safety
///
/// The IIO core calls it with a device registered by [`TempIio`].
unsafe extern "C" fn buffer_postenable(indio_dev: *mut bindings::iio_dev) -> c_int {
    // SAFETY: The private data is the `Arc<State>` stored by `TempIio::new()`.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    match state
        .control
        .set_buffer(true, || state.sample.lock().valid = false)
    {
        Ok(()) => {
            *state.push_enabled.lock() = true;
            0
        }
        Err(e) => e.to_errno(),
    }
}

/// Stops the reports when the buffer is disabled.
///
/// # Safety
///
/// As [`buffer_postenable`].
unsafe extern "C" fn buffer_predisable(indio_dev: *mut bindings::iio_dev) -> c_int {
    // SAFETY: As in `buffer_postenable()`.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    // Synchronously drain any in-flight buffer push before IIO core detaches
    // the buffer list, while keeping direct-read sample caching active.
    *state.push_enabled.lock() = false;
    match state.control.set_buffer(false, || {}) {
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

fn temp_channel() -> bindings::iio_chan_spec {
    bindings::iio_chan_spec {
        type_: bindings::BINDINGS_IIO_TEMP,
        scan_index: 0,
        __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
            scan_type: bindings::iio_scan_type {
                __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 { sign: b's' },
                realbits: 32,
                storagebits: 32,
                ..Default::default()
            },
        },
        info_mask_separate: 1usize << bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED,
        info_mask_shared_by_type: 1usize << bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ,
        ..Default::default()
    }
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

/// Owns the IIO device and what it points to so it is released with the
/// platform device's resources, after the devres kfifo buffer is detached.
struct IioAlloc {
    dev: *mut bindings::iio_dev,
    _spec: Pin<KBox<Spec>>,
}

impl Drop for IioAlloc {
    fn drop(&mut self) {
        // SAFETY: `self.dev` was allocated in `TempIio::new()` with an
        // `Arc<State>` as its private data. `TempIio` has unregistered it and
        // resources registered after this one (the kfifo buffer) are released.
        unsafe {
            drop(Arc::<State>::from_foreign((*self.dev).priv_.cast()));
            bindings::iio_device_free(self.dev);
        }
    }
}

// SAFETY: The IIO device may be freed from any thread, and the channel
// table is only read.
unsafe impl Send for IioAlloc {}

struct TempIio {
    dev: *mut bindings::iio_dev,
    registered: Atomic<bool>,
}

impl TempIio {
    fn new(dev: &device::Device<Bound>, control: Arc<Control>) -> Result<(Self, Arc<State>)> {
        let spec = KBox::pin(
            Spec {
                channels: [temp_channel(), timestamp_channel()],
                info: bindings::iio_info {
                    read_raw: Some(read_raw),
                    ..Default::default()
                },
                _p: PhantomPinned,
            },
            GFP_KERNEL,
        )?;
        // SAFETY: `dev` is valid.
        let indio_dev = unsafe { bindings::iio_device_alloc(dev.as_raw(), 0) };
        if indio_dev.is_null() {
            return Err(ENOMEM);
        }
        let state = match Arc::pin_init(
            pin_init!(State {
                iio: IioPtr(indio_dev),
                control,
                sample <- new_spinlock!(Sample::default()),
                push_enabled <- new_spinlock!(false),
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
            (*indio_dev).name = c"aop-sensors-als-temp".as_char_ptr();
            (*indio_dev).channels = spec.channels.as_ptr();
            (*indio_dev).num_channels = spec.channels.len() as i32;
            (*indio_dev).info = &spec.info;
            (*indio_dev).modes = bindings::INDIO_DIRECT_MODE as i32;
            (*indio_dev).available_scan_masks = SCAN_MASKS.as_ptr();
        }
        // Register the IIO allocation before the kfifo buffer so devres
        // detaches the buffer before freeing `indio_dev`.
        devres::register(
            dev,
            IioAlloc {
                dev: indio_dev,
                _spec: spec,
            },
            GFP_KERNEL,
        )?;
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
            return Err(Error::from_errno(ret));
        }
        Ok((
            Self {
                dev: indio_dev,
                registered: Atomic::new(false),
            },
            state,
        ))
    }

    fn register(&self) -> Result {
        let module = kernel::module::this_module::<crate::LocalModule>();
        // SAFETY: The device is fully initialized and unregistered.
        let ret = unsafe { bindings::__iio_device_register(self.dev, module.as_ptr()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        self.registered.store(true, Relaxed);
        Ok(())
    }

    fn unregister(&self) {
        if self.registered.xchg(false, Relaxed) {
            // SAFETY: `self.dev` was registered by `register()`. It is freed
            // with the platform device's resources, which outlive this.
            unsafe { bindings::iio_device_unregister(self.dev) };
        }
    }
}

impl Drop for TempIio {
    fn drop(&mut self) {
        self.unregister();
    }
}

// SAFETY: The IIO core serializes registration and removal.
unsafe impl Send for TempIio {}
// SAFETY: As above.
unsafe impl Sync for TempIio {}

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
    // SAFETY: `nb` is the first field of a registered, pinned `PmHook`.
    let hook = unsafe { &*nb.cast::<PmHook>() };
    let invalidate = || hook.state.sample.lock().valid = false;
    let ret = match action as u32 {
        bindings::PM_SUSPEND_PREPARE
        | bindings::PM_HIBERNATION_PREPARE
        | bindings::PM_RESTORE_PREPARE => hook.state.control.set_parked(true, false, || {}),
        bindings::PM_POST_SUSPEND => {
            // SAFETY: Reads the PM core's sleep-transition flag, which stays
            // set until the next suspend entry.
            let fw_sleep = unsafe { bindings::pm_suspend_via_firmware() };
            hook.state.control.set_parked(false, fw_sleep, invalidate)
        }
        bindings::PM_POST_HIBERNATION | bindings::PM_POST_RESTORE => {
            hook.state.control.set_parked(false, true, invalidate)
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

struct IIOAopTempDriver {
    iio: TempIio,
    pm: Pin<KBox<PmHook>>,
    control: Arc<Control>,
    listener: FakehidRegistration,
}

impl IIOAopTempDriver {
    fn teardown(&self) {
        // Unregister IIO first so no new read_raw or buffer callback can start,
        // then unregister PM, stop reports and permanently drain delayed work,
        // and finally remove the fake-HID listener.
        self.iio.unregister();
        self.pm.unregister();
        let _ = self.control.shutdown();
        self.listener.unregister();
    }
}

impl Drop for IIOAopTempDriver {
    fn drop(&mut self) {
        self.teardown();
    }
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-als-temp")), ())]
);

impl platform::Driver for IIOAopTempDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopTempDriver;

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
        let control = Control::new(dev, aop.clone(), service)?;
        let (iio, state) = TempIio::new(dev, control.clone())?;
        let listener = FakehidRegistration::new(aop, service, state.clone())?;
        let pm = PmHook::new(state)?;
        if let Err(e) = control.probe_init() {
            pm.unregister();
            let _ = control.shutdown();
            return Err(e);
        }
        let drv = IIOAopTempDriver {
            iio,
            pm,
            control,
            listener,
        };
        drv.iio.register()?;
        Ok(drv)
    }

    fn unbind(_dev: &platform::Device<Core<'_>>, this: Pin<&Self>) {
        this.teardown();
    }
}

/// The AOP core creates the device of a service, named after the service,
/// whenever the firmware announces that service. Registering the driver under
/// the same name lets the platform bus bind it on every machine whose AOP
/// offers the service, with or without a device tree node for it.
const SERVICE_NAME: &CStr = c_str!("als-temp");

#[pin_data]
struct IIOAopTempModule {
    #[pin]
    _driver: driver::Registration<platform::Adapter<IIOAopTempDriver>>,
}

impl kernel::InPlaceModule for IIOAopTempModule {
    fn init(module: &'static ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _driver <- driver::Registration::new(SERVICE_NAME, module),
        })
    }
}

module! {
    type: IIOAopTempModule,
    name: "iio_aop_temp",
    description: "AOP temperature sensor driver",
    license: "Dual MIT/GPL",
    alias: ["platform:als-temp"],
}
