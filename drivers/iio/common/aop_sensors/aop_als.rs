// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP ambient light sensor driver
//!
//! Copyright (C) The Asahi Linux Contributors
//!
//! Reports run on demand when sysfs reads arrive and stop again once reads go
//! quiet for a short idle window, so the AOP does not wake the application
//! processor at 5 Hz while nothing reads illuminance or colour temperature.
//! Calibration data for the VD6286 and CT720 is loaded once at probe and sent
//! once per AOP boot (and conservatively again after firmware sleep,
//! hibernation, or restore). The CT817 arms through its full configuration
//! sequence on every start.

use core::{
    ffi::{c_int, c_void},
    marker::PhantomPinned,
    ptr,
};

use kernel::{
    bindings, c_str,
    device::{self, Bound, Core},
    devres,
    firmware::Firmware,
    iio::common::aop_sensors::{MessageProcessor, MAX_MICRO, MICRO},
    module_platform_driver, new_delayed_work, new_mutex, new_spinlock, of, platform,
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
};

const EPIC_SUBTYPE_GET_AOP_PROPERTY: u16 = 0xa;
const EPIC_SUBTYPE_SET_ALS_PROPERTY: u16 = 0x4;

/* ALS properties. */
const ALS_PROP_INTERVAL: u32 = 0x00;
const ALS_PROP_CALIBRATION: u32 = 0x0b;
const ALS_PROP_BATCH_INTERVAL: u32 = 0x16;
const ALS_PROP_RAW_INTERVAL: u32 = 0x5c;
const ALS_PROP_VERBOSITY: u32 = 0xe1;

/*
 * The CT817 starts reporting on the zero to non-zero transition of its
 * report interval, and it accepts that transition only once its batch and
 * raw interval properties have been written. Despite their names these two
 * are not intervals; they are the values the sensor is configured with on
 * the T8140. The sensor answers every one of these writes with a
 * not-permitted status and applies them regardless, so the status is only
 * logged.
 */
const ALS_BATCH_INTERVAL: u32 = 1;
const ALS_RAW_INTERVAL: u32 = 0;
const ALS_VERBOSITY: u32 = 6;
/// The reporting interval in microseconds (5 Hz).
const ALS_INTERVAL_US: u32 = 200_000;
/// How long a direct read waits for the first 5 Hz report of a fresh start.
const FIRST_SAMPLE_MS: i64 = 1_000;
const FIRST_SAMPLE_POLL_MS: i64 = 20;
/// How long reports stay on after the last direct read.
const DIRECT_IDLE_MS: i64 = 5_000;
const MAX_CHANNELS: usize = 2;

const LUX_OFFSET_CT720: usize = 0x1d;
// These offsets address the fake-HID EPIC payload, not a host HID event.
const LUX_OFFSET_VD6286: usize = 0x28;
// The documented 122-byte Redbird report carries firmware-computed Kelvin.
// Other report layouts must be decoded separately, even for the same sensor.
const VD6286_COLOR_REPORT_LEN: usize = 122;
const CCT_OFFSET_VD6286: usize = 0x45;
/*
 * The CT817 sends a 32-byte report: two raw channel floats at +0x08 and
 * +0x0c and the lux float at +0x1c, one byte below the CT720's, so the two
 * parts do not share a layout.
 */
const LUX_OFFSET_CT817: usize = 0x1c;

fn get_lux_offset(aop: &dyn AOP, dev: &platform::Device, svc: &EPICService) -> Result<usize> {
    let name = get_aop_property(aop, svc, 0xf, 16)?.1;
    match name.as_slice() {
        b"Redbird\0" => Ok(LUX_OFFSET_VD6286),
        b"FireFish2\0" => Ok(LUX_OFFSET_CT720),
        // The T8140 AOP reports the part number rather than a code name.
        b"CT817\0" => Ok(LUX_OFFSET_CT817),
        _ => {
            dev_warn!(
                dev.as_ref(),
                "Unknown sensor type {:?}",
                core::str::from_utf8(&name)
            );
            Err(EIO)
        }
    }
}

/// Arms the CT817: interval zero, its configuration, then the interval that
/// starts the reports.
fn arm_ct817(aop: &dyn AOP, dev: &device::Device, svc: &EPICService) -> Result<()> {
    // The CT817 gets its calibration from the AOP driver over the setup port,
    // not as an ALS property. Only the transport can fail here; the sensor's
    // status words are logged, see the property comments above. Do not fetch
    // the HID report descriptor (subtype 0x01) on this endpoint: the AOP never
    // answers it, and do not write the MODE (0xd7) or 0xe4 properties: either
    // wedges the AOP for the rest of the boot.
    let set = |tag: u32, val: u32, what: &str| -> Result<()> {
        let status = set_als_property(aop, svc, tag, &val.to_le_bytes())?;
        dev_dbg!(dev, "{} = {:#x}: status {:#x}\n", what, val, status);
        Ok(())
    };
    // The sensor arms only on a zero to non-zero transition of the interval.
    // It is zero at boot and stopped after a sleep, but a previous driver
    // instance may have left it running, so write zero first every time.
    set(ALS_PROP_INTERVAL, 0, "interval")?;
    set(ALS_PROP_VERBOSITY, ALS_VERBOSITY, "verbosity")?;
    set(
        ALS_PROP_BATCH_INTERVAL,
        ALS_BATCH_INTERVAL,
        "batch interval",
    )?;
    set(ALS_PROP_RAW_INTERVAL, ALS_RAW_INTERVAL, "raw interval")?;
    // The transition that arms the sensor.
    set(ALS_PROP_INTERVAL, ALS_INTERVAL_US, "interval")
}

/// What a stream state change requires from the service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SyncAction {
    None,
    /// Enable reports; send calibration first when `calibrate` is true.
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
    /// reasserts the desired state in either direction.
    Unknown,
}

/// Firmware transport operations needed by stream lifecycle transitions.
trait AlsIo {
    fn calibrate(&self) -> Result;
    fn start_reports(&self) -> Result;
    fn stop_reports(&self) -> Result;
}

/// On-demand stream state for ambient light reports.
struct Stream<T> {
    /// Calibration has been sent in this AOP boot (or is not needed, as on CT817).
    calibrated: bool,
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
            direct_readers: 0,
            direct_until: None,
            failed_until: None,
            parked: false,
            run_state: RunState::Stopped,
        }
    }

    fn want(&self) -> bool {
        !self.parked && (self.direct_readers > 0 || self.direct_until.is_some())
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
        if self.parked {
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

    /// Arms a rate-limited idle-stop retry whenever an unacknowledged start or
    /// stop leaves an idle stream potentially running.
    fn arm_stop_retry(&mut self, deadline: T) -> bool {
        if !self.want() && self.run_state != RunState::Stopped {
            self.direct_until = Some(deadline);
            true
        } else {
            false
        }
    }

    /// Verifies calibration, arming, and stopping once at probe, leaving the
    /// sensor idle-stopped until the first direct read.
    fn probe_with(&mut self, io: &dyn AlsIo) -> Result {
        io.calibrate()?;
        self.calibrated = true;
        match io.start_reports() {
            Ok(()) => {
                self.run_state = RunState::Running;
            }
            Err(e) => {
                // An unacknowledged start may still have taken effect in
                // firmware; mark unknown and attempt one best-effort stop.
                self.run_state = RunState::Unknown;
                if io.stop_reports().is_ok() {
                    self.run_state = RunState::Stopped;
                }
                return Err(e);
            }
        }
        match io.stop_reports() {
            Ok(()) => {
                self.run_state = RunState::Stopped;
            }
            Err(_) => {
                self.run_state = RunState::Unknown;
                if io.stop_reports().is_ok() {
                    self.run_state = RunState::Stopped;
                }
            }
        }
        Ok(())
    }

    /// Starts or stops the reports to match active readers. `before_start`
    /// runs right before enabling reports so any cached sample from an earlier
    /// run is invalidated before new reports arrive.
    fn sync_with(&mut self, io: &dyn AlsIo, before_start: impl FnOnce()) -> Result {
        match self.next_action() {
            SyncAction::Start { calibrate } => {
                if calibrate {
                    io.calibrate()?;
                    self.calibrated = true;
                }
                before_start();
                match io.start_reports() {
                    Ok(()) => {
                        self.run_state = RunState::Running;
                        Ok(())
                    }
                    Err(e) => {
                        self.run_state = RunState::Unknown;
                        Err(e)
                    }
                }
            }
            SyncAction::Stop => match io.stop_reports() {
                Ok(()) => {
                    self.run_state = RunState::Stopped;
                    Ok(())
                }
                Err(e) => {
                    self.run_state = RunState::Unknown;
                    Err(e)
                }
            },
            SyncAction::None => Ok(()),
        }
    }

    /// Parks reports across system sleep and restores them on wake only if a
    /// reader still wants them. Idle sensors stay stopped across sleep/wake.
    fn set_parked_with(
        &mut self,
        io: &dyn AlsIo,
        parked: bool,
        reset_cal: bool,
        ct817: bool,
        retry_deadline: T,
        before_start: impl FnOnce(),
    ) -> (Result, bool) {
        self.parked = parked;
        self.failed_until = None;
        if parked {
            self.direct_until = None;
        } else if reset_cal {
            self.calibrated = false;
        } else if ct817 && self.run_state == RunState::Running {
            // Defensive: firmware stops the CT817 for system sleep even if the
            // host did not park it first.
            self.run_state = RunState::Stopped;
        }
        let ret = self.sync_with(io, before_start);
        let retry = !parked && ret.is_err() && self.arm_stop_retry(retry_deadline);
        (ret, retry)
    }

    fn shutdown_with(&mut self, io: &dyn AlsIo) -> Result {
        self.parked = true;
        self.direct_until = None;
        self.failed_until = None;
        self.sync_with(io, || {})
    }
}

#[pin_data]
struct Control {
    dev: ARef<device::Device>,
    aop: Arc<dyn AOP>,
    service: EPICService,
    /// The part arms only through the full CT817 sequence.
    ct817: bool,
    /// Calibration blob loaded once at probe for non-CT817 sensors.
    calibration: Option<KVec<u8>>,
    /// Avoids spamming dmesg on repeated idle-stop retries if AOP stalls.
    stop_warned: Atomic<bool>,
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

impl AlsIo for Control {
    fn calibrate(&self) -> Result {
        if self.ct817 {
            return Ok(());
        }
        if let Some(ref cal) = self.calibration {
            let status =
                set_als_property(self.aop.as_ref(), &self.service, ALS_PROP_CALIBRATION, cal)?;
            dev_dbg!(self.dev, "calibration: status {:#x}\n", status);
        }
        Ok(())
    }

    fn start_reports(&self) -> Result {
        if self.ct817 {
            return arm_ct817(self.aop.as_ref(), &self.dev, &self.service);
        }
        let status = set_als_property(
            self.aop.as_ref(),
            &self.service,
            ALS_PROP_INTERVAL,
            &ALS_INTERVAL_US.to_le_bytes(),
        )?;
        dev_dbg!(
            self.dev,
            "interval = {:#x}: status {:#x}\n",
            ALS_INTERVAL_US,
            status
        );
        Ok(())
    }

    fn stop_reports(&self) -> Result {
        let status = set_als_property(
            self.aop.as_ref(),
            &self.service,
            ALS_PROP_INTERVAL,
            &0u32.to_le_bytes(),
        )?;
        dev_dbg!(self.dev, "interval = 0: status {:#x}\n", status);
        Ok(())
    }
}

impl Control {
    fn new(
        dev: &device::Device,
        aop: Arc<dyn AOP>,
        service: EPICService,
        ct817: bool,
    ) -> Result<Arc<Self>> {
        let calibration = if ct817 {
            None
        } else {
            // The installer collects the calibration blob from macOS, so it is
            // not present on every system. Without it the VD6286 of the M4 Pro
            // never reports and its channel reads ENODATA. Load it once at
            // probe so wake and resume paths never touch the filesystem, and
            // warn once if missing.
            match Firmware::request_nowarn(c_str!("apple/aop-als-cal.bin"), dev) {
                Ok(fw) => {
                    let mut owned = KVec::new();
                    owned.extend_from_slice(fw.data(), GFP_KERNEL)?;
                    Some(owned)
                }
                Err(e) => {
                    dev_warn!(
                        dev,
                        "no apple/aop-als-cal.bin ({:?}); the sensor may not report\n",
                        e
                    );
                    None
                }
            }
        };
        Arc::pin_init(
            pin_init!(Self {
                dev: dev.into(),
                aop,
                service,
                ct817,
                calibration,
                stop_warned: Atomic::new(false),
                stream <- new_mutex!(Stream::new()),
                idle_work <- new_delayed_work!("Control::idle_work"),
            }),
            GFP_KERNEL,
        )
    }

    fn probe_init(self: &Arc<Self>) -> Result {
        let retry = {
            let mut s = self.stream.lock();
            s.probe_with(self.as_ref())?;
            if s.run_state == RunState::Unknown {
                let retry_deadline =
                    Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
                s.arm_stop_retry(retry_deadline)
            } else {
                false
            }
        };
        if retry {
            dev_warn!(
                self.dev,
                "reports not stopped after probe; will retry after idle\n"
            );
            self.schedule_idle(DIRECT_IDLE_MS as u32);
        }
        Ok(())
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

    fn set_parked(
        self: &Arc<Self>,
        parked: bool,
        reset_cal: bool,
        before_start: impl FnOnce(),
    ) -> Result {
        let (ret, retry) = {
            let mut s = self.stream.lock();
            let retry_deadline = Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
            s.set_parked_with(
                self.as_ref(),
                parked,
                reset_cal,
                self.ct817,
                retry_deadline,
                before_start,
            )
        };
        if parked {
            self.stop_idle_sync(false);
        } else if retry {
            self.schedule_idle(DIRECT_IDLE_MS as u32);
        }
        ret
    }

    fn shutdown(self: &Arc<Self>) -> Result {
        self.stop_idle_sync(true);
        let mut s = self.stream.lock();
        s.shutdown_with(self.as_ref())
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
                IdleStep::Expire => match s.sync_with(this.as_ref(), || {}) {
                    Ok(()) => {
                        this.stop_warned.store(false, Relaxed);
                        None
                    }
                    Err(e) => {
                        if !this.stop_warned.xchg(true, Relaxed) {
                            dev_warn!(this.dev, "reports not stopped after idle: {:?}\n", e);
                        }
                        let retry_deadline =
                            Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
                        if s.arm_stop_retry(retry_deadline) {
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

/// Stops any active reports around system sleep and keeps them off on wake
/// while nothing is reading the sensor.
#[repr(C)]
struct PmHook {
    /// First, so that the callback finds the hook from it.
    nb: Opaque<bindings::notifier_block>,
    state: Arc<State>,
    registered: Atomic<bool>,
}

/// The PM notifier callback.
///
/// # Safety
///
/// `nb` is the first field of a registered [`PmHook`].
unsafe extern "C" fn pm_event(
    nb: *mut bindings::notifier_block,
    action: usize,
    _data: *mut c_void,
) -> c_int {
    // SAFETY: `nb` is the first field of a registered `PmHook`.
    let hook = unsafe { &*nb.cast::<PmHook>() };
    let invalidate = || hook.state.invalidate_samples();
    let ret = match action as u32 {
        // A sensor that keeps reporting only costs wakeups: never refuse
        // the sleep over it.
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

/// # Safety
///
/// `nb` stays valid and pinned until [`pm_unregister`] is called on it.
#[cfg(CONFIG_PM_SLEEP)]
unsafe fn pm_register(nb: *mut bindings::notifier_block) -> c_int {
    // SAFETY: By the caller's contract.
    unsafe { bindings::register_pm_notifier(nb) }
}

/// # Safety
///
/// `nb` was registered by [`pm_register`].
#[cfg(CONFIG_PM_SLEEP)]
unsafe fn pm_unregister(nb: *mut bindings::notifier_block) {
    // SAFETY: By the caller's contract.
    unsafe { bindings::unregister_pm_notifier(nb) };
}

/// Without system sleep there is nothing to switch.
///
/// # Safety
///
/// None; the signature matches the one with system sleep.
#[cfg(not(CONFIG_PM_SLEEP))]
unsafe fn pm_register(_nb: *mut bindings::notifier_block) -> c_int {
    0
}

/// # Safety
///
/// None; the signature matches the one with system sleep.
#[cfg(not(CONFIG_PM_SLEEP))]
unsafe fn pm_unregister(_nb: *mut bindings::notifier_block) {}

// SAFETY: The notifier block is only touched by the PM core, which
// serializes registration, removal and the callbacks itself.
unsafe impl Send for PmHook {}
// SAFETY: See above; the other fields are `Sync`.
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
        // SAFETY: The block is pinned inside the hook, which unregisters it
        // before it goes.
        let ret = unsafe { pm_register(hook.nb.get()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        hook.registered.store(true, Relaxed);
        Ok(hook)
    }

    /// Returns once no callback is running any more; repeated calls do
    /// nothing.
    fn unregister(&self) {
        if self.registered.xchg(false, Relaxed) {
            // SAFETY: `nb` was registered by `new()`.
            unsafe { pm_unregister(self.nb.get()) };
        }
    }
}

impl Drop for PmHook {
    fn drop(&mut self) {
        self.unregister();
    }
}

fn get_aop_property(
    aop: &dyn AOP,
    svc: &EPICService,
    tag: u32,
    data_len: usize,
) -> Result<(u32, KVec<u8>)> {
    let mut buf = KVec::new();
    buf.resize(8, 0, GFP_KERNEL)?;
    buf[4..8].copy_from_slice(&tag.to_le_bytes());
    aop.epic_call_ret(svc, EPIC_SUBTYPE_GET_AOP_PROPERTY, &buf, data_len)
}

fn set_als_property(aop: &dyn AOP, svc: &EPICService, tag: u32, data: &[u8]) -> Result<u32> {
    let mut buf = KVec::new();
    buf.resize(data.len() + 8, 0, GFP_KERNEL)?;
    buf[8..].copy_from_slice(data);
    buf[4..8].copy_from_slice(&tag.to_le_bytes());
    aop.epic_call(svc, EPIC_SUBTYPE_SET_ALS_PROPERTY, &buf)
}

/// Converts the bits of an IEEE 754 single to microlux, rounding down.
/// Negative values clamp to zero; NaN and the infinities carry no reading.
/// Values beyond `MAX_MICRO` saturate when they are stored.
fn f32_to_micro(f: u32) -> Option<u64> {
    let exp = ((f >> 23) & 0xff) as i32;
    if exp == 0xff {
        return None;
    }
    if f & 0x8000_0000 != 0 || exp == 0 {
        return Some(0);
    }
    // The value is mant * 2^shift; scaled to microlux it is below 2^44
    // before the shift, so a left shift of up to 19 cannot overflow.
    let mant = u64::from(f & 0x7f_ffff | 0x80_0000) * MICRO;
    let shift = exp - 127 - 23;
    Some(if shift >= 20 {
        u64::MAX
    } else if shift >= 0 {
        mant << shift
    } else if shift <= -64 {
        0
    } else {
        mant >> -shift
    })
}

struct MsgProc(usize);

fn report_float(message: &[u8], offset: usize) -> Option<u64> {
    let raw = message.get(offset..offset + 4)?;
    f32_to_micro(u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]))
}

impl MessageProcessor for MsgProc {
    fn process(&self, message: &[u8]) -> Option<u64> {
        report_float(message, self.0)
    }

    fn process_channel(&self, message: &[u8], channel: usize) -> Option<u64> {
        match channel {
            0 => self.process(message),
            1 if self.0 == LUX_OFFSET_VD6286 && message.len() == VD6286_COLOR_REPORT_LEN => {
                // Zero and negative CCT do not describe a colour temperature.
                report_float(message, CCT_OFFSET_VD6286).filter(|value| *value != 0)
            }
            _ => None,
        }
    }
}

enum AlsIio {
    Light(AlsIioDev),
    Color(AlsIioDev),
}

impl AlsIio {
    fn register(&self) -> Result {
        match self {
            Self::Light(dev) | Self::Color(dev) => dev.register(),
        }
    }

    fn unregister(&self) {
        match self {
            Self::Light(dev) | Self::Color(dev) => dev.unregister(),
        }
    }
}

#[derive(Clone, Copy, Default)]
struct Sample {
    value: [u64; MAX_CHANNELS],
    valid: [bool; MAX_CHANNELS],
    seen_report: bool,
}

#[pin_data]
struct State {
    control: Arc<Control>,
    msg_proc: MsgProc,
    ty: [u32; MAX_CHANNELS],
    num_channels: usize,
    #[pin]
    sample: SpinLock<Sample>,
}

/// Keeps `direct_readers` accounted for across `read_now()`, and arms the
/// idle-stop deadline once the reader finishes.
struct DirectReadGuard<'a>(&'a Arc<Control>);

impl Drop for DirectReadGuard<'_> {
    fn drop(&mut self) {
        let schedule = {
            let mut s = self.0.stream.lock();
            s.direct_readers = s.direct_readers.saturating_sub(1);
            if !s.parked {
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
    fn invalidate_samples(&self) {
        *self.sample.lock() = Sample::default();
    }

    /// Returns the latest reading in micro-units for `channel`. Reports are
    /// started on demand and kept running until direct reads go quiet for
    /// `DIRECT_IDLE_MS`.
    fn read_now(&self, channel: usize) -> Result<u64> {
        let (already_seen, guard) = {
            let mut s = self.control.stream.lock();
            if s.parked {
                return Err(EBUSY);
            }
            if let Some((until, _)) = s.failed_until {
                let remaining_ms = (until - Instant::<Monotonic>::now()).as_millis_ceil();
                if let Some(e) = s.check_backoff(remaining_ms) {
                    return Err(e);
                }
            }
            let was_running = s.run_state == RunState::Running;
            s.direct_readers += 1;
            if let Err(e) = s.sync_with(self.control.as_ref(), || self.invalidate_samples()) {
                s.direct_readers = s.direct_readers.saturating_sub(1);
                let until = Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
                s.failed_until = Some((until, e));
                let retry = s.arm_stop_retry(until);
                drop(s);
                if retry {
                    self.control.schedule_idle(DIRECT_IDLE_MS as u32);
                }
                return Err(e);
            }
            let already_seen = was_running && self.sample.lock().seen_report;
            (already_seen, DirectReadGuard(&self.control))
        };

        let mut waited = 0;
        while waited < FIRST_SAMPLE_MS {
            let sample = *self.sample.lock();
            if sample.valid[channel] || (already_seen && sample.seen_report) {
                break;
            }
            if self.control.stream.lock().parked {
                return Err(EBUSY);
            }
            fsleep(Delta::from_millis(FIRST_SAMPLE_POLL_MS));
            waited += FIRST_SAMPLE_POLL_MS;
        }
        let sample = *self.sample.lock();
        drop(guard);
        if sample.valid[channel] {
            Ok(sample.value[channel])
        } else {
            Err(ENODATA)
        }
    }
}

impl FakehidListener for State {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()> {
        let mut decoded = [None; MAX_CHANNELS];
        let mut any = false;
        for (channel, slot) in decoded.iter_mut().enumerate().take(self.num_channels) {
            if let Some(value) = self.msg_proc.process_channel(data, channel) {
                *slot = Some(value.min(MAX_MICRO));
                any = true;
            }
        }
        if any {
            let mut sample = self.sample.lock();
            for (channel, slot) in decoded.iter().enumerate().take(self.num_channels) {
                if let Some(value) = *slot {
                    sample.value[channel] = value;
                    sample.valid[channel] = true;
                }
            }
            sample.seen_report = true;
        }
        Ok(())
    }
}

/// # Safety
///
/// The IIO core supplies the registered device, channel, and output pointers.
unsafe extern "C" fn als_read_raw(
    indio_dev: *mut bindings::iio_dev,
    chan: *const bindings::iio_chan_spec,
    val: *mut i32,
    val2: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: `priv_` is the `Arc<State>` installed by `AlsIioDev::new()`.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    // SAFETY: `chan` is one of the registered channel specs.
    let (ty, channel) = unsafe { ((*chan).type_, (*chan).address as usize) };
    if channel >= state.num_channels || state.ty[channel] != ty {
        return EINVAL.to_errno();
    }
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED as isize {
        return EINVAL.to_errno();
    }
    let micro = match state.read_now(channel) {
        Ok(value) => value,
        Err(e) => return e.to_errno(),
    };
    // Both parts fit an `i32` because stored readings never exceed `MAX_MICRO`.
    let (int, frac) = ((micro / MICRO) as i32, (micro % MICRO) as i32);
    // SAFETY: The pointers are the caller's result variables.
    unsafe {
        *val = int;
        *val2 = frac;
    }
    bindings::IIO_VAL_INT_PLUS_MICRO as i32
}

struct Spec<const N: usize> {
    channels: [bindings::iio_chan_spec; N],
    info: bindings::iio_info,
    _p: PhantomPinned,
}

struct IioAlloc<const N: usize> {
    dev: *mut bindings::iio_dev,
    _spec: Pin<KBox<Spec<N>>>,
}

impl<const N: usize> Drop for IioAlloc<N> {
    fn drop(&mut self) {
        // SAFETY: `self.dev` was allocated in `AlsIioDev::new()` with an
        // `Arc<State>` as its private data and has been unregistered.
        unsafe {
            drop(Arc::<State>::from_foreign((*self.dev).priv_.cast()));
            bindings::iio_device_free(self.dev);
        }
    }
}

// SAFETY: The IIO device may be freed from any thread, and the channel
// table is only read.
unsafe impl<const N: usize> Send for IioAlloc<N> {}

struct AlsIioDev {
    dev: *mut bindings::iio_dev,
    registered: Atomic<bool>,
}

impl AlsIioDev {
    fn new<const N: usize>(
        dev: &device::Device<Bound>,
        control: Arc<Control>,
        offset: usize,
        types: [u32; N],
    ) -> Result<(Self, Arc<State>)> {
        if N == 0 || N > MAX_CHANNELS {
            return Err(EINVAL);
        }
        let info_mask = 1usize << bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED;
        let spec = KBox::pin(
            Spec::<N> {
                channels: core::array::from_fn(|channel| bindings::iio_chan_spec {
                    type_: types[channel],
                    address: channel as _,
                    __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
                        scan_type: bindings::iio_scan_type {
                            __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 {
                                format: b'u' as _,
                            },
                            realbits: 32,
                            storagebits: 32,
                            ..Default::default()
                        },
                    },
                    info_mask_separate: info_mask,
                    ..Default::default()
                }),
                info: bindings::iio_info {
                    read_raw: Some(als_read_raw),
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
        let mut ty = [0u32; MAX_CHANNELS];
        ty[..N].copy_from_slice(&types);
        let state = match Arc::pin_init(
            pin_init!(State {
                control,
                msg_proc: MsgProc(offset),
                ty,
                num_channels: N,
                sample <- new_spinlock!(Sample::default()),
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
            (*indio_dev).name = c"aop-sensors-als".as_char_ptr();
            (*indio_dev).channels = spec.channels.as_ptr();
            (*indio_dev).num_channels = spec.channels.len() as i32;
            (*indio_dev).info = &spec.info;
        }
        devres::register(
            dev,
            IioAlloc {
                dev: indio_dev,
                _spec: spec,
            },
            GFP_KERNEL,
        )?;
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
            // SAFETY: `self.dev` was registered by `register()` and is freed
            // with the platform device's resources.
            unsafe { bindings::iio_device_unregister(self.dev) };
        }
    }
}

impl Drop for AlsIioDev {
    fn drop(&mut self) {
        self.unregister();
    }
}

// SAFETY: The IIO core serializes registration and removal.
unsafe impl Send for AlsIioDev {}
// SAFETY: As above.
unsafe impl Sync for AlsIioDev {}

struct IIOAopAlsDriver {
    iio: AlsIio,
    pm: Pin<KBox<PmHook>>,
    control: Arc<Control>,
    listener: FakehidRegistration,
}

impl IIOAopAlsDriver {
    fn teardown(&self) {
        // Unregister IIO first so no new read_raw can start, then unregister
        // PM, drain delayed work and stop reports, and finally remove the
        // fake-HID listener.
        self.iio.unregister();
        self.pm.unregister();
        if let Err(e) = self.control.shutdown() {
            dev_warn!(self.control.dev, "reports not stopped at unbind: {:?}\n", e);
        }
        self.listener.unregister();
    }
}

impl Drop for IIOAopAlsDriver {
    fn drop(&mut self) {
        self.teardown();
    }
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-als")), ())]
);

impl platform::Driver for IIOAopAlsDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopAlsDriver;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound ()>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let dev = pdev.as_ref();
        // SAFETY: This binds only to the service devices the AOP core
        // registers, and `pdev` is being probed.
        let adata = unsafe { <dyn AOP>::from_child(dev) }?;
        // SAFETY: `dev` is a valid device; the AOP core attached its service
        // record as the platform data of every service device it registers.
        let service = unsafe { (*dev.as_raw()).platform_data.cast::<EPICService>() };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: The record lives as long as the device it is attached to.
        let service = unsafe { *service };
        let ty = bindings::BINDINGS_IIO_LIGHT;
        let offset = get_lux_offset(adata.as_ref(), pdev, &service)?;
        let control = Control::new(dev, adata.clone(), service, offset == LUX_OFFSET_CT817)?;
        let (iio, state) = if offset == LUX_OFFSET_VD6286 {
            let (iio_dev, state) = AlsIioDev::new(
                dev,
                control.clone(),
                offset,
                [ty, bindings::BINDINGS_IIO_COLORTEMP],
            )?;
            (AlsIio::Color(iio_dev), state)
        } else {
            let (iio_dev, state) = AlsIioDev::new(dev, control.clone(), offset, [ty])?;
            (AlsIio::Light(iio_dev), state)
        };
        let listener = FakehidRegistration::new(adata, service, state.clone())?;
        let pm = PmHook::new(state)?;
        if let Err(e) = control.probe_init() {
            pm.unregister();
            let _ = control.shutdown();
            return Err(e);
        }
        let drv = IIOAopAlsDriver {
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

module_platform_driver! {
    type: IIOAopAlsDriver,
    name: "iio_aop_als",
    description: "AOP ambient light sensor driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_als"],
    firmware: ["apple/aop-als-cal.bin"],
}
