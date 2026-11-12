// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! System sleep. A Touch ID capture runs on a non-freezable workqueue and keeps
//! talking to the sensor over SPI, so one left running into a suspend is still
//! active while devices go down. A J313 that suspended with the lock screen's
//! capture in flight did not wake. Before the system suspends, end any capture
//! in progress and hold new ones until it has resumed. The hwrng fill thread
//! is not frozen either, so entropy requests are held over the same window.
//! The notifier itself is in `pm_shim.c`; the capture and entropy handling is
//! on [`SepData`].
//!
//! On a firmware-driven suspend the enclave also saves its state and gates its
//! own power domain when asked, as the sleep powers it down, and has to be
//! started again after a wake. See [`SleepGate`] and `SepData::sleep_enter`.

use core::sync::atomic::{AtomicPtr, Ordering};

use kernel::prelude::*;
use kernel::sync::Arc;
use kernel::types::ForeignOwnable;

use crate::SepData;

/// The notifier has no context pointer, so `SepData` is reached through this
/// global, as in `trusted.rs`. Holds the [`ForeignOwnable::into_foreign`]
/// pointer while registered; `null` otherwise.
static SEP: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());

extern "C" {
    fn sep_pm_register(
        dev: *mut c_void,
        event: unsafe extern "C" fn(bool),
        sleep: unsafe extern "C" fn() -> c_int,
        wake: unsafe extern "C" fn(),
        told: unsafe extern "C" fn() -> bool,
    ) -> c_int;
    fn sep_pm_unregister();
    fn sep_pm_keep_domains(dev: *mut c_void) -> c_int;
    fn sep_pm_clear_dev_disable(dev: *mut c_void) -> c_int;
    fn sep_pm_dart_restore(dev: *mut c_void);
    fn sep_pm_dart_prepare(dev: *mut c_void) -> c_int;
    fn sep_pm_wait_gated(dev: *mut c_void, timeout_ms: c_uint) -> c_int;
    fn sep_pm_platform_commit() -> bool;
    fn sep_pm_commit_prepare(
        dev: *mut c_void,
        mbox: *mut c_void,
        msg0: u64,
        msg1: u32,
        reply: u64,
        mask: u64,
    ) -> c_int;
    fn sep_pm_commit_finish(dev: *mut c_void) -> c_int;
    pub(crate) static sep_pm_ops: kernel::bindings::dev_pm_ops;
}

/// Who may use the mailbox around a firmware-driven suspend.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SleepGate {
    /// Anyone.
    Open = 0,
    /// Replies to what the enclave asked, and the sleep request itself.
    Draining = 1,
    /// Nobody until the wake: the enclave is gated or about to be.
    Told = 2,
    /// Nobody: a previous wake failed. New firmware sleeps refuse until reboot.
    Lost = 3,
}

impl SleepGate {
    pub(crate) fn from_u32(v: u32) -> SleepGate {
        match v {
            0 => SleepGate::Open,
            1 => SleepGate::Draining,
            2 => SleepGate::Told,
            _ => SleepGate::Lost,
        }
    }

    pub(crate) fn may_request(self) -> bool {
        self == SleepGate::Open
    }

    pub(crate) fn may_reply(self) -> bool {
        matches!(self, SleepGate::Open | SleepGate::Draining)
    }

    /// The enclave may be gated, so a wake has to look.
    pub(crate) fn needs_wake(self) -> bool {
        matches!(self, SleepGate::Told | SleepGate::Lost)
    }
}

/// Stages `request` for the irreversible commit, with RX held until physical wake.
/// Unsupported providers/transports refuse here; device suspend never sends SLEEP.
pub(crate) fn leave_for_commit(
    dev: &kernel::device::Device,
    mbox: *mut kernel::bindings::apple_mbox,
    request: kernel::soc::apple::mailbox::Message,
    reply: u64,
    mask: u64,
) -> Result<()> {
    // SAFETY: `dev` is the bound SEP device and `mbox` its mailbox, which outlives the sleep:
    // `commit_finish` takes the request back at the resume.
    let rc = unsafe {
        sep_pm_commit_prepare(
            dev.as_raw().cast(),
            mbox.cast(),
            request.msg0,
            request.msg1,
            reply,
            mask,
        )
    };
    if rc == 0 {
        Ok(())
    } else {
        Err(Error::from_errno(rc))
    }
}

/// Finishes a staged request. A sent request requires a real firmware return;
/// the C boundary terminates an impossible sent-without-sleep unwind. `None` if unstaged.
pub(crate) fn commit_finish(dev: &kernel::device::Device) -> Option<bool> {
    // SAFETY: `dev` is the bound SEP device.
    let rc = unsafe { sep_pm_commit_finish(dev.as_raw().cast()) };
    if rc < 0 {
        None
    } else {
        Some(rc > 0)
    }
}

/// Puts the enclave's DART back if it lost its state, before the doorbell.
/// How long the enclave gets to gate its domain once it has answered (told at suspend).
const GATE_TIMEOUT_MS: u32 = 5000;

/// Whether a sleep core commit sends the staged SLEEP; if not, it is sent at device suspend.
pub(crate) fn platform_commit() -> bool {
    // SAFETY: only reads the sleep core's state.
    unsafe { sep_pm_platform_commit() }
}

/// Whether the enclave gated itself within the bound.
pub(crate) fn wait_gated(dev: &kernel::device::Device) -> bool {
    // SAFETY: `dev` is the bound SEP device.
    unsafe { sep_pm_wait_gated(dev.as_raw().cast(), GATE_TIMEOUT_MS) == 0 }
}

/// Saves the enclave's DART while it can be read, before the enclave is told.
pub(crate) fn dart_prepare(dev: &kernel::device::Device) -> Result<()> {
    // SAFETY: `dev` is the bound SEP device.
    let rc = unsafe { sep_pm_dart_prepare(dev.as_raw().cast()) };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }
    Ok(())
}

pub(crate) fn dart_restore(dev: &kernel::device::Device) {
    // SAFETY: `dev` is the bound SEP device.
    unsafe { sep_pm_dart_restore(dev.as_raw().cast()) };
}

/// Lets a gated enclave start again. Writes only if the bit is set.
pub(crate) fn clear_dev_disable(dev: &kernel::device::Device) -> Result<()> {
    // SAFETY: `dev` is the bound SEP device.
    let rc = unsafe { sep_pm_clear_dev_disable(dev.as_raw().cast()) };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }
    Ok(())
}

/// Attaches every power domain of a multi-domain SEP node and keeps them on
/// across system sleep; see `pm_shim.c`. Must run before the enclave is first
/// spoken to.
pub(crate) fn keep_domains(dev: &kernel::device::Device) -> Result<()> {
    // SAFETY: `dev` is the bound SEP device; the shim ties what it attaches to
    // that device's lifetime through devres.
    let rc = unsafe { sep_pm_keep_domains(dev.as_raw().cast()) };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }
    Ok(())
}

/// The holds a sleep event takes and releases, on [`SepData`].
trait SleepHolds {
    /// Holds entropy requests, once any in flight is answered.
    fn hold_entropy(&self);
    /// Ends any Touch ID capture and holds new ones.
    fn hold_captures(&self);
    fn release_captures(&self);
    fn release_entropy(&self);
}

impl SleepHolds for SepData {
    fn hold_entropy(&self) {
        self.rng_sleep_prepare();
    }
    fn hold_captures(&self) {
        self.sleep_prepare()
    }
    fn release_captures(&self) {
        self.sleep_finished();
    }
    fn release_entropy(&self) {
        self.rng_sleep_finished();
    }
}

/// `entering`: the system is about to sleep; otherwise it has resumed, or
/// the suspend was aborted.
fn sleep_event(sep: &impl SleepHolds, entering: bool) {
    if entering {
        sep.hold_entropy();
        sep.hold_captures();
    } else {
        sep.release_captures();
        sep.release_entropy();
    }
}

/// # Safety
/// Called by `pm_shim.c` from the PM notifier chain, in process context.
unsafe extern "C" fn pm_event(entering: bool) {
    let ptr = SEP.load(Ordering::Acquire);
    if ptr.is_null() {
        return;
    }
    // SAFETY: produced by `into_foreign` in `register`, and reclaimed by
    // `unregister` only after `sep_pm_unregister` has waited out any running
    // notifier call, so it is live for this borrow.
    let sep = unsafe { <Arc<SepData> as ForeignOwnable>::borrow(ptr) };
    sleep_event(&*sep, entering);
}

/// Runs `f` on the registered instance. Only for callbacks that
/// `sep_pm_unregister` waits for or that the bound device excludes.
fn with_sep<R>(none: R, f: impl FnOnce(&SepData) -> R) -> R {
    let ptr = SEP.load(Ordering::Acquire);
    if ptr.is_null() {
        return none;
    }
    // SAFETY: as in `pm_event`.
    let sep = unsafe { <Arc<SepData> as ForeignOwnable>::borrow(ptr) };
    f(&*sep)
}

/// # Safety
/// Called by `pm_shim.c` from the device's suspend callback.
unsafe extern "C" fn pm_sleep() -> c_int {
    with_sep(0, |sep| match sep.sleep_enter() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    })
}

/// # Safety
/// Called by `pm_shim.c` from the device's resume callback.
unsafe extern "C" fn pm_wake() {
    with_sep((), |sep| sep.sleep_exit());
}

/// # Safety
/// Called by `pm_shim.c`, whether or not the device suspended.
unsafe extern "C" fn pm_told() -> bool {
    with_sep(false, |sep| sep.sleep_gate() == SleepGate::Told)
}

pub(crate) fn register(sep: Arc<SepData>) -> Result<()> {
    let dev: *mut c_void = sep.dev.as_raw().cast();
    let ptr = sep.into_foreign();

    if SEP
        .compare_exchange(
            core::ptr::null_mut(),
            ptr,
            Ordering::AcqRel,
            Ordering::Acquire,
        )
        .is_err()
    {
        // SAFETY: `ptr` was just produced by `into_foreign` and never published.
        drop(unsafe { <Arc<SepData> as ForeignOwnable>::from_foreign(ptr) });
        return Err(EBUSY);
    }

    // SAFETY: the shim stores the callback and registers a notifier block
    // that lives for the module's lifetime.
    let rc = unsafe { sep_pm_register(dev, pm_event, pm_sleep, pm_wake, pm_told) };
    if rc != 0 {
        let raw = SEP.swap(core::ptr::null_mut(), Ordering::AcqRel);
        if !raw.is_null() {
            // SAFETY: `raw` is the pointer we published; no notifier is wired.
            drop(unsafe { <Arc<SepData> as ForeignOwnable>::from_foreign(raw) });
        }
        return Err(Error::from_errno(rc));
    }
    Ok(())
}

/// Safe to call even if [`register`] never ran. Unregisters the notifier,
/// which waits for a running call to finish, before reclaiming the `Arc`.
pub(crate) fn unregister() {
    if SEP.load(Ordering::Acquire).is_null() {
        return;
    }
    // SAFETY: no preconditions; matched with `sep_pm_register` above.
    unsafe { sep_pm_unregister() };

    let ptr = SEP.swap(core::ptr::null_mut(), Ordering::AcqRel);
    if !ptr.is_null() {
        // SAFETY: `ptr` came from `into_foreign` in `register`, and the
        // notifier can no longer call `pm_event`.
        drop(unsafe { <Arc<SepData> as ForeignOwnable>::from_foreign(ptr) });
    }
}

#[kernel::macros::kunit_tests(apple_sep_pm)]
mod tests {
    use super::*;
    use core::cell::{Cell, RefCell};

    const HOLD_ENTROPY: u8 = 1;
    const HOLD_CAPTURES: u8 = 2;
    const RELEASE_CAPTURES: u8 = 3;
    const RELEASE_ENTROPY: u8 = 4;

    struct Calls {
        made: RefCell<[u8; 8]>,
        count: Cell<usize>,
    }

    impl Calls {
        fn new() -> Self {
            Calls {
                made: RefCell::new([0; 8]),
                count: Cell::new(0),
            }
        }
        fn call(&self, which: u8) {
            let n = self.count.get();
            self.made.borrow_mut()[n] = which;
            self.count.set(n + 1);
        }
        fn made(&self) -> [u8; 8] {
            *self.made.borrow()
        }
    }

    impl SleepHolds for Calls {
        fn hold_entropy(&self) {
            self.call(HOLD_ENTROPY);
        }
        fn hold_captures(&self) {
            self.call(HOLD_CAPTURES);
        }
        fn release_captures(&self) {
            self.call(RELEASE_CAPTURES);
        }
        fn release_entropy(&self) {
            self.call(RELEASE_ENTROPY);
        }
    }

    #[test]
    fn the_gate_lets_replies_out_until_the_enclave_is_told() {
        assert!(SleepGate::Open.may_request());
        assert!(SleepGate::Open.may_reply());
        assert!(!SleepGate::Draining.may_request());
        assert!(SleepGate::Draining.may_reply());
        assert!(!SleepGate::Told.may_request());
        assert!(!SleepGate::Told.may_reply());
        assert!(!SleepGate::Lost.may_request());
        assert!(!SleepGate::Lost.may_reply());
    }

    #[test]
    fn only_a_told_or_lost_enclave_needs_waking() {
        assert!(!SleepGate::Open.needs_wake());
        assert!(!SleepGate::Draining.needs_wake());
        assert!(SleepGate::Told.needs_wake());
        assert!(SleepGate::Lost.needs_wake());
    }

    #[test]
    fn the_gate_round_trips() {
        for gate in [SleepGate::Open, SleepGate::Draining, SleepGate::Told, SleepGate::Lost] {
            assert_eq!(SleepGate::from_u32(gate as u32), gate);
        }
    }

    #[test]
    fn going_to_sleep_holds_entropy_then_captures() {
        let calls = Calls::new();
        sleep_event(&calls, true);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, 0, 0, 0, 0, 0, 0]
        );
    }

    #[test]
    fn sleep_holds_stay_until_the_post_event() {
        let calls = Calls::new();
        sleep_event(&calls, true);
        assert_eq!(calls.count.get(), 2);
        sleep_event(&calls, false);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, RELEASE_CAPTURES, RELEASE_ENTROPY, 0, 0, 0, 0]
        );
    }

    #[test]
    fn waking_releases_captures_then_entropy() {
        let calls = Calls::new();
        sleep_event(&calls, false);
        assert_eq!(
            calls.made(),
            [RELEASE_CAPTURES, RELEASE_ENTROPY, 0, 0, 0, 0, 0, 0]
        );
    }
}
