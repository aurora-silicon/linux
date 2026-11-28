// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP lid angle sensor driver
//!
//! Copyright (C) The Asahi Linux Contributors
//!
//! When the service supports reading and clearing its report interval (and
//! reports within `MAX_ONDEMAND_INTERVAL_US`), reports are stopped at probe
//! and run only while direct sysfs reads are active (with a short idle window
//! so polling callers share one stream). Services whose interval cannot be
//! queried or stopped, or whose period is longer than the on-demand bound,
//! keep the passive streaming behaviour and still stop across system sleep
//! when readable.

use core::{
    ffi::{c_int, c_void},
    marker::PhantomPinned,
    ptr,
};

use kernel::{
    bindings, c_str,
    device::{self, Bound, Core},
    devres,
    iio::common::aop_sensors::{MessageProcessor, MICRO},
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

const SUBTYPE_SET_PROPERTY: u16 = 4;
const SUBTYPE_GET_PROPERTY: u16 = 10;
/// The report interval, microseconds; 0 stops the reports.
const PROP_INTERVAL: u32 = 0;
/// Maximum firmware period (2 s) eligible for on-demand cold-start waits.
/// Slower services stay passive so direct reads never time out waiting for a
/// multi-second first report.
const MAX_ONDEMAND_INTERVAL_US: u32 = 2_000_000;
/// Polling cadence while waiting for the first report of a fresh start.
const FIRST_SAMPLE_POLL_MS: i64 = 20;
/// How long reports stay on after the last direct read.
const DIRECT_IDLE_MS: i64 = 5_000;

/// Writes one u32 property; returns the service's status word.
fn set_u32(aop: &dyn AOP, svc: &EPICService, prop: u32, value: u32) -> Result<u32> {
    let mut msg = KVec::with_capacity(12, GFP_KERNEL)?;
    msg.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL)?;
    msg.extend_from_slice(&prop.to_le_bytes(), GFP_KERNEL)?;
    msg.extend_from_slice(&value.to_le_bytes(), GFP_KERNEL)?;
    aop.epic_call(svc, SUBTYPE_SET_PROPERTY, &msg)
}

/// Reads one u32 property; `None` when the service refuses or answers short.
fn get_u32(aop: &dyn AOP, svc: &EPICService, prop: u32) -> Result<Option<u32>> {
    let mut msg = KVec::new();
    msg.resize(8, 0, GFP_KERNEL)?;
    msg[4..8].copy_from_slice(&prop.to_le_bytes());
    let (status, value) = aop.epic_call_ret(svc, SUBTYPE_GET_PROPERTY, &msg, 4)?;
    if status != 0 || value.len() < 4 {
        return Ok(None);
    }
    Ok(Some(u32::from_le_bytes([
        value[0], value[1], value[2], value[3],
    ])))
}

/// How long a direct read waits for the first sample after starting `interval_us`.
fn first_sample_wait_ms(interval_us: u32) -> i64 {
    i64::from(interval_us.div_ceil(1_000)) + 500
}

/// What a stream state change requires from the service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SyncAction {
    None,
    Start(u32),
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

/// Firmware property transport needed by stream lifecycle transitions.
trait IntervalIo {
    fn get_interval(&self) -> Result<Option<u32>>;
    fn set_interval_status(&self, interval: u32) -> Result<u32>;
    fn set_interval(&self, interval: u32) -> Result;
}

/// Outcome of [`Stream::probe_with`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ProbeEvent {
    IdleStopped(u32),
    StopRefused(u32),
    StopTimedOutStopped(Error),
    StopFailedStillRunning(Error, u32),
    StopAmbiguousRestored(Error, u32),
    StopAmbiguousRecoverable(Error, u32),
    ExceedsBound(u32),
    AlreadyZero,
    NotReadable,
    ReadFailed(Error),
}

/// On-demand stream state for lid angle reports.
struct Stream<T> {
    /// Firmware's default interval when on-demand gating is supported (`None`
    /// when the service cannot be queried/stopped or is slower than
    /// `MAX_ONDEMAND_INTERVAL_US`, leaving passive streaming).
    interval_us: Option<u32>,
    /// Pre-probe firmware interval to restore on unbind or probe rollback
    /// whenever probe attempted to clear `PROP_INTERVAL`, preserved until
    /// restoration is confirmed.
    restore_interval: Option<u32>,
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
            interval_us: None,
            restore_interval: None,
            direct_readers: 0,
            direct_until: None,
            failed_until: None,
            parked: false,
            run_state: RunState::Stopped,
        }
    }

    fn want(&self) -> bool {
        self.interval_us.is_some()
            && !self.parked
            && (self.direct_readers > 0 || self.direct_until.is_some())
    }

    fn next_action(&self) -> SyncAction {
        let Some(interval) = self.interval_us else {
            return SyncAction::None;
        };
        match (self.want(), self.run_state) {
            (true, RunState::Stopped | RunState::Unknown) => SyncAction::Start(interval),
            (false, RunState::Running | RunState::Unknown) => SyncAction::Stop,
            _ => SyncAction::None,
        }
    }

    fn check_idle(&mut self, remaining_ms: i64) -> IdleStep {
        if self.direct_until.is_none() {
            return IdleStep::Ignore;
        }
        if self.parked || self.interval_us.is_none() {
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

    fn check_backoff(&mut self, remaining_ms: i64) -> Option<Error> {
        if remaining_ms <= 0 {
            self.failed_until = None;
        }
        self.failed_until.as_ref().map(|(_, e)| *e)
    }

    /// Arms a rate-limited idle-stop retry whenever an unacknowledged start or
    /// stop leaves an idle on-demand stream potentially running.
    fn arm_stop_retry(&mut self, deadline: T) -> bool {
        if self.interval_us.is_some() && !self.want() && self.run_state != RunState::Stopped {
            self.direct_until = Some(deadline);
            true
        } else {
            false
        }
    }

    fn sync_with(&mut self, io: &dyn IntervalIo) -> Result {
        match self.next_action() {
            SyncAction::Start(interval) => match io.set_interval(interval) {
                Ok(()) => {
                    self.run_state = RunState::Running;
                    Ok(())
                }
                Err(e) => {
                    self.run_state = RunState::Unknown;
                    Err(e)
                }
            },
            SyncAction::Stop => match io.set_interval(0) {
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

    /// Queries the firmware's reporting interval and stops idle streaming when
    /// the service reports within `MAX_ONDEMAND_INTERVAL_US` and accepts
    /// `PROP_INTERVAL = 0`.
    fn probe_with(&mut self, io: &dyn IntervalIo, retry_deadline: T) -> (ProbeEvent, bool) {
        match io.get_interval() {
            Ok(Some(interval)) if interval > 0 && interval <= MAX_ONDEMAND_INTERVAL_US => {
                match io.set_interval_status(0) {
                    Ok(0) => {
                        self.interval_us = Some(interval);
                        self.restore_interval = Some(interval);
                        self.run_state = RunState::Stopped;
                        (ProbeEvent::IdleStopped(interval), false)
                    }
                    Ok(status) => (ProbeEvent::StopRefused(status), false),
                    Err(e) => match io.get_interval() {
                        // The stop write may have taken effect before the reply
                        // timed out. Reconcile the live interval and preserve
                        // the original period so unbind/rebind never inherits 0.
                        Ok(Some(0)) => {
                            self.interval_us = Some(interval);
                            self.restore_interval = Some(interval);
                            self.run_state = RunState::Stopped;
                            (ProbeEvent::StopTimedOutStopped(e), false)
                        }
                        Ok(Some(cur)) if cur > 0 => {
                            (ProbeEvent::StopFailedStillRunning(e, cur), false)
                        }
                        _ => match io.set_interval_status(interval) {
                            Ok(0) => (ProbeEvent::StopAmbiguousRestored(e, interval), false),
                            _ => {
                                // Both readback and passive restoration failed;
                                // keep on-demand mode from Unknown so direct
                                // reads restart reports and idle retry stops
                                // them instead of stranding passive ENODATA.
                                self.interval_us = Some(interval);
                                self.restore_interval = Some(interval);
                                self.run_state = RunState::Unknown;
                                let retry = self.arm_stop_retry(retry_deadline);
                                (ProbeEvent::StopAmbiguousRecoverable(e, interval), retry)
                            }
                        },
                    },
                }
            }
            Ok(Some(interval)) if interval > MAX_ONDEMAND_INTERVAL_US => {
                (ProbeEvent::ExceedsBound(interval), false)
            }
            Ok(Some(_)) => (ProbeEvent::AlreadyZero, false),
            Ok(None) => (ProbeEvent::NotReadable, false),
            Err(e) => (ProbeEvent::ReadFailed(e), false),
        }
    }

    fn shutdown_with(&mut self, io: &dyn IntervalIo) -> Result {
        self.parked = true;
        self.direct_until = None;
        self.failed_until = None;
        if let Some(interval) = self.restore_interval {
            io.set_interval(interval)?;
            self.run_state = RunState::Running;
            self.restore_interval = None;
            self.interval_us = None;
        }
        Ok(())
    }
}

#[pin_data]
struct Control {
    dev: ARef<device::Device>,
    aop: Arc<dyn AOP>,
    service: EPICService,
    #[pin]
    stream: Mutex<Stream<Instant<Monotonic>>>,
    #[pin]
    idle_work: DelayedWork<Control>,
}

impl_has_delayed_work! {
    impl HasDelayedWork<Self> for Control { self.idle_work }
}

impl IntervalIo for Control {
    fn get_interval(&self) -> Result<Option<u32>> {
        get_u32(self.aop.as_ref(), &self.service, PROP_INTERVAL)
    }

    fn set_interval_status(&self, interval: u32) -> Result<u32> {
        set_u32(self.aop.as_ref(), &self.service, PROP_INTERVAL, interval)
    }

    fn set_interval(&self, interval: u32) -> Result {
        match self.set_interval_status(interval)? {
            0 => Ok(()),
            status => {
                dev_err!(
                    self.dev,
                    "interval {} us refused: status {:#x}\n",
                    interval,
                    status
                );
                Err(EIO)
            }
        }
    }
}

impl Control {
    fn new(dev: &device::Device, aop: Arc<dyn AOP>, service: EPICService) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                dev: dev.into(),
                aop,
                service,
                stream <- new_mutex!(Stream::new()),
                idle_work <- new_delayed_work!("Control::idle_work"),
            }),
            GFP_KERNEL,
        )
    }

    /// Queries the firmware's reporting interval and stops idle streaming when
    /// the service reports within `MAX_ONDEMAND_INTERVAL_US` and accepts
    /// `PROP_INTERVAL = 0`.
    fn probe_init(self: &Arc<Self>) {
        let (event, retry) = {
            let mut s = self.stream.lock();
            let retry_deadline = Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS);
            s.probe_with(self.as_ref(), retry_deadline)
        };
        match event {
            ProbeEvent::IdleStopped(interval) => dev_dbg!(
                self.dev,
                "reports idle-stopped (interval {} us)\n",
                interval
            ),
            ProbeEvent::StopRefused(status) => dev_info!(
                self.dev,
                "idle stop refused (status {:#x}); leaving reports on\n",
                status
            ),
            ProbeEvent::StopTimedOutStopped(e) => dev_dbg!(
                self.dev,
                "idle stop timed out ({:?}) but interval is 0; using on-demand mode\n",
                e
            ),
            ProbeEvent::StopFailedStillRunning(e, cur) => dev_warn!(
                self.dev,
                "idle stop failed ({:?}); interval still {} us, leaving passive\n",
                e,
                cur
            ),
            ProbeEvent::StopAmbiguousRestored(e, interval) => dev_warn!(
                self.dev,
                "idle stop ambiguous ({:?}); restored {} us, leaving passive\n",
                e,
                interval
            ),
            ProbeEvent::StopAmbiguousRecoverable(e, interval) => dev_warn!(
                self.dev,
                "idle stop ambiguous ({:?}) and restore of {} us failed; keeping on-demand mode from unknown state\n",
                e,
                interval
            ),
            ProbeEvent::ExceedsBound(interval) => dev_dbg!(
                self.dev,
                "interval {} us exceeds on-demand bound; leaving reports passive\n",
                interval
            ),
            ProbeEvent::AlreadyZero => dev_dbg!(self.dev, "interval is already 0 at probe\n"),
            ProbeEvent::NotReadable => {
                dev_dbg!(self.dev, "interval not readable; leaving reports passive\n")
            }
            ProbeEvent::ReadFailed(e) => {
                dev_warn!(self.dev, "interval read failed at probe: {:?}\n", e)
            }
        }
        if retry {
            self.schedule_idle(DIRECT_IDLE_MS as u32);
        }
    }

    fn sync_locked(&self, s: &mut Stream<Instant<Monotonic>>) -> Result {
        s.sync_with(self)
    }

    /// Arms a rate-limited idle-stop retry whenever an unacknowledged start or
    /// stop leaves an idle on-demand stream potentially running.
    fn arm_stop_retry_locked(&self, s: &mut Stream<Instant<Monotonic>>) -> bool {
        s.arm_stop_retry(Instant::<Monotonic>::now() + Delta::from_millis(DIRECT_IDLE_MS))
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

    fn set_parked(self: &Arc<Self>, parked: bool, fw_reset: bool) -> Result {
        let (ret, retry) = {
            let mut s = self.stream.lock();
            if s.interval_us.is_none() {
                return Ok(());
            }
            s.parked = parked;
            s.failed_until = None;
            if parked {
                s.direct_until = None;
            } else if fw_reset {
                // Firmware sleep or hibernation may restart the AOP with its
                // default streaming interval; stop it again while nobody reads.
                s.run_state = RunState::Unknown;
            }
            let ret = self.sync_locked(&mut s);
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

    /// Permanently drains delayed work and unconditionally restores the
    /// pre-probe firmware interval so a subsequent rebind observes the
    /// hardware's period even after an unacknowledged idle stop. The saved
    /// interval is cleared only once restoration succeeds so a second teardown
    /// pass (for example `Drop` after `unbind`) can retry transient failures.
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
                IdleStep::Expire => match this.sync_locked(&mut s) {
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

/// Stops any active lid angle reports around system sleep and keeps them off
/// after wake while nobody reads them.
#[repr(C)]
struct PmHook {
    /// First, so that the callback finds the hook from it.
    nb: Opaque<bindings::notifier_block>,
    control: Arc<Control>,
    /// Fallback interval to restore after sleep when on-demand mode is not
    /// active; 0 when nothing was stopped on the fallback path.
    saved: Atomic<u32>,
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
    let on_demand = hook.control.stream.lock().interval_us.is_some();
    if on_demand {
        let ret = match action as u32 {
            bindings::PM_SUSPEND_PREPARE
            | bindings::PM_HIBERNATION_PREPARE
            | bindings::PM_RESTORE_PREPARE => hook.control.set_parked(true, false),
            bindings::PM_POST_SUSPEND => {
                // SAFETY: Reads the PM core's sleep-transition flag.
                let fw_sleep = unsafe { bindings::pm_suspend_via_firmware() };
                hook.control.set_parked(false, fw_sleep)
            }
            bindings::PM_POST_HIBERNATION | bindings::PM_POST_RESTORE => {
                hook.control.set_parked(false, true)
            }
            _ => return bindings::NOTIFY_DONE as c_int,
        };
        if let Err(e) = ret {
            dev_warn!(
                hook.control.dev,
                "reports not switched for sleep: {:?}\n",
                e
            );
        }
        return bindings::NOTIFY_OK as c_int;
    }

    let aop = hook.control.aop.as_ref();
    match action as u32 {
        bindings::PM_SUSPEND_PREPARE => match get_u32(aop, &hook.control.service, PROP_INTERVAL) {
            Ok(Some(0)) => {}
            Ok(Some(interval)) => match set_u32(aop, &hook.control.service, PROP_INTERVAL, 0) {
                Ok(status) => {
                    hook.saved.store(interval, Relaxed);
                    dev_info!(
                        hook.control.dev,
                        "reports off for sleep (interval {} us): status {:#x}\n",
                        interval,
                        status
                    );
                }
                Err(e) => dev_warn!(hook.control.dev, "reports not stopped for sleep: {:?}\n", e),
            },
            Ok(None) => dev_info!(
                hook.control.dev,
                "interval not readable: reports left on for sleep\n"
            ),
            Err(e) => dev_warn!(hook.control.dev, "interval read failed: {:?}\n", e),
        },
        bindings::PM_POST_SUSPEND => {
            let interval = hook.saved.xchg(0, Relaxed);
            if interval != 0 {
                match set_u32(aop, &hook.control.service, PROP_INTERVAL, interval) {
                    Ok(status) => dev_info!(
                        hook.control.dev,
                        "reports back after sleep (interval {} us): status {:#x}\n",
                        interval,
                        status
                    ),
                    Err(e) => dev_warn!(
                        hook.control.dev,
                        "reports not restarted after sleep: {:?}\n",
                        e
                    ),
                }
            }
        }
        _ => return bindings::NOTIFY_DONE as c_int,
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
    fn new(control: Arc<Control>) -> Result<Pin<KBox<Self>>> {
        let hook = KBox::pin(
            Self {
                nb: Opaque::new(bindings::notifier_block {
                    notifier_call: Some(pm_event),
                    next: ptr::null_mut(),
                    priority: 0,
                }),
                control,
                saved: Atomic::new(0),
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

struct MsgProc;

impl MessageProcessor for MsgProc {
    fn process(&self, message: &[u8]) -> Option<u64> {
        // Report 1 contains a little-endian, 9-bit angle in degrees.
        // Other report IDs carry sensor properties, not angle samples.
        if *message.first()? != 1 {
            return None;
        }
        let angle = u16::from_le_bytes([*message.get(1)?, *message.get(2)?]) & 0x1ff;
        if angle > 360 {
            return None;
        }
        Some(u64::from(angle) * MICRO)
    }

    fn scale(&self) -> Option<(i32, i32)> {
        // IIO_ANGL uses radians: one raw count is pi / 180 radians.
        Some((0, 17_453_293))
    }
}

#[derive(Clone, Copy, Default)]
struct Sample {
    angle: i32,
    valid: bool,
}

#[pin_data]
struct State {
    control: Arc<Control>,
    #[pin]
    sample: SpinLock<Sample>,
}

impl FakehidListener for State {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()> {
        if let Some(micro) = MsgProc.process(data) {
            *self.sample.lock() = Sample {
                angle: (micro / MICRO) as i32,
                valid: true,
            };
        }
        Ok(())
    }
}

/// Keeps `direct_readers` accounted for across `read_now()`, and arms the
/// idle-stop deadline once the reader finishes.
struct DirectReadGuard<'a>(&'a Arc<Control>);

impl Drop for DirectReadGuard<'_> {
    fn drop(&mut self) {
        let schedule = {
            let mut s = self.0.stream.lock();
            s.direct_readers = s.direct_readers.saturating_sub(1);
            if s.interval_us.is_some() && !s.parked {
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
    fn read_now(&self) -> Result<i32> {
        let (max_wait, guard) = {
            let mut s = self.control.stream.lock();
            let Some(interval) = s.interval_us else {
                drop(s);
                let sample = *self.sample.lock();
                return if sample.valid {
                    Ok(sample.angle)
                } else {
                    Err(ENODATA)
                };
            };
            if s.parked {
                return Err(EBUSY);
            }
            if let Some((until, _)) = s.failed_until {
                let remaining_ms = (until - Instant::<Monotonic>::now()).as_millis_ceil();
                if let Some(e) = s.check_backoff(remaining_ms) {
                    return Err(e);
                }
            }
            if s.run_state != RunState::Running {
                // A sample from an earlier start may be stale.
                self.sample.lock().valid = false;
            }
            s.direct_readers += 1;
            if let Err(e) = self.control.sync_locked(&mut s) {
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
            (
                first_sample_wait_ms(interval),
                DirectReadGuard(&self.control),
            )
        };

        let mut waited = 0;
        while waited < max_wait && !self.sample.lock().valid {
            if self.control.stream.lock().parked {
                return Err(EBUSY);
            }
            fsleep(Delta::from_millis(FIRST_SAMPLE_POLL_MS));
            waited += FIRST_SAMPLE_POLL_MS;
        }
        let sample = *self.sample.lock();
        drop(guard);
        if sample.valid {
            Ok(sample.angle)
        } else {
            Err(ENODATA)
        }
    }
}

/// # Safety
///
/// The IIO core supplies the registered device and output pointers.
unsafe extern "C" fn las_read_raw(
    indio_dev: *mut bindings::iio_dev,
    chan: *const bindings::iio_chan_spec,
    val: *mut i32,
    val2: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: `priv_` is the `Arc<State>` installed by `LasIio::new()`.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    // SAFETY: `chan` is the registered channel spec.
    if unsafe { (*chan).type_ != bindings::BINDINGS_IIO_ANGL || (*chan).address != 0 } {
        return EINVAL.to_errno();
    }
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_SCALE as isize {
        let Some((int, frac)) = MsgProc.scale() else {
            return EINVAL.to_errno();
        };
        // SAFETY: The pointers are the caller's result variables.
        unsafe {
            *val = int;
            *val2 = frac;
        }
        return bindings::IIO_VAL_INT_PLUS_NANO as i32;
    }
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_RAW as isize {
        return EINVAL.to_errno();
    }
    let angle = match state.read_now() {
        Ok(angle) => angle,
        Err(e) => return e.to_errno(),
    };
    // SAFETY: The pointers are the caller's result variables.
    unsafe {
        *val = angle;
        *val2 = 0;
    }
    bindings::IIO_VAL_INT as i32
}

struct Spec {
    channels: [bindings::iio_chan_spec; 1],
    info: bindings::iio_info,
    _p: PhantomPinned,
}

struct IioAlloc {
    dev: *mut bindings::iio_dev,
    _spec: Pin<KBox<Spec>>,
}

impl Drop for IioAlloc {
    fn drop(&mut self) {
        // SAFETY: `self.dev` was allocated in `LasIio::new()` with an
        // `Arc<State>` as its private data and has been unregistered.
        unsafe {
            drop(Arc::<State>::from_foreign((*self.dev).priv_.cast()));
            bindings::iio_device_free(self.dev);
        }
    }
}

// SAFETY: The IIO device may be freed from any thread, and the channel
// table is only read.
unsafe impl Send for IioAlloc {}

struct LasIio {
    dev: *mut bindings::iio_dev,
    registered: Atomic<bool>,
}

impl LasIio {
    fn new(dev: &device::Device<Bound>, control: Arc<Control>) -> Result<(Self, Arc<State>)> {
        let info_mask = (1usize << bindings::BINDINGS_IIO_CHAN_INFO_RAW)
            | (1usize << bindings::BINDINGS_IIO_CHAN_INFO_SCALE);
        let spec = KBox::pin(
            Spec {
                channels: [bindings::iio_chan_spec {
                    type_: bindings::BINDINGS_IIO_ANGL,
                    address: 0,
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
                }],
                info: bindings::iio_info {
                    read_raw: Some(las_read_raw),
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
                control,
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
            (*indio_dev).name = c"aop-sensors-las".as_char_ptr();
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

impl Drop for LasIio {
    fn drop(&mut self) {
        self.unregister();
    }
}

// SAFETY: The IIO core serializes registration and removal.
unsafe impl Send for LasIio {}
// SAFETY: As above.
unsafe impl Sync for LasIio {}

struct IIOAopLasDriver {
    iio: LasIio,
    pm: Pin<KBox<PmHook>>,
    control: Arc<Control>,
    listener: FakehidRegistration,
}

impl IIOAopLasDriver {
    fn teardown(&self) {
        // Unregister IIO first so no new read_raw can start, then unregister
        // PM, drain delayed work and restore the pre-probe firmware interval,
        // and finally remove the fake-HID listener.
        self.iio.unregister();
        self.pm.unregister();
        let _ = self.control.shutdown();
        self.listener.unregister();
    }
}

impl Drop for IIOAopLasDriver {
    fn drop(&mut self) {
        self.teardown();
    }
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-las")), ())]
);

impl platform::Driver for IIOAopLasDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopLasDriver;

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
        // SAFETY: The record lives as long as this child device.
        let service = unsafe { *service };

        let control = Control::new(dev, adata.clone(), service)?;
        let (iio, state) = LasIio::new(dev, control.clone())?;
        let listener = FakehidRegistration::new(adata, service, state)?;
        let pm = PmHook::new(control.clone())?;
        control.probe_init();
        let drv = IIOAopLasDriver {
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
    type: IIOAopLasDriver,
    name: "iio_aop_las",
    description: "AOP lid angle sensor driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_las"],
}
