// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! System sleep. A Touch ID capture runs on a non-freezable workqueue and keeps
//! talking to the sensor over SPI, so one left running into a suspend is still
//! active while devices go down. A J313 that suspended with the lock screen's
//! capture in flight did not wake. Before the system suspends, end any capture
//! in progress and hold new ones until it has resumed. The hwrng fill thread
//! is not frozen either, so entropy requests are held over the same window.
//!
//! A hibernation also holds every request Linux starts, and the enclave's
//! xART writes are refused, from its prepare until the kernel knows how it
//! ended. The image must not miss host or gigalocker state written after the
//! snapshot, and after a resume from disk on a cold boot the enclave is a
//! freshly booted sepOS that must not see a message of the old session before
//! the new registration. The PM core tells a resume from the image from an
//! aborted hibernation or the wake of a hybrid sleep, and the PMP, which the
//! boot loader halts on a cold boot, whether that resume went through one
//! (`pm_shim.c`); the device is then probed again.
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
        event: unsafe extern "C" fn(c_int) -> c_int,
        sleep: unsafe extern "C" fn() -> c_int,
        wake: unsafe extern "C" fn(),
        told: unsafe extern "C" fn() -> bool,
    ) -> c_int;
    fn sep_pm_unregister();
    fn sep_pm_keep_domains(dev: *mut c_void) -> c_int;
    fn sep_pm_clear_dev_disable(dev: *mut c_void) -> c_int;
    fn sep_pm_dart_restore(dev: *mut c_void);
    fn sep_pm_commit_prepare(
        dev: *mut c_void,
        mbox: *mut c_void,
        msg0: u64,
        msg1: u32,
        reply: u64,
        mask: u64,
    ) -> c_int;
    fn sep_pm_commit_finish(dev: *mut c_void) -> c_int;
    fn sep_pm_exit();
    pub(crate) static sep_pm_ops: kernel::bindings::dev_pm_ops;
}

// Sleep events from `pm_shim.c`; must match `SEP_PM_*` in `shim.h`.
const EVENT_SLEEP: c_int = 0;
const EVENT_WAKE: c_int = 1;
const EVENT_HIBERNATE: c_int = 2;
const EVENT_HIBERNATE_WARM: c_int = 3;
const EVENT_HIBERNATE_COLD: c_int = 4;
const EVENT_RESTORE: c_int = 5;

/// Cancels a probe queued after a resume from disk. Called once the module
/// goes away, before the driver is unregistered.
pub(crate) fn exit() {
    // SAFETY: no preconditions; waits for a running reprobe to finish.
    unsafe { sep_pm_exit() };
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
    /// Holds every request Linux starts and refuses the enclave's xART
    /// writes, or lifts that, around a hibernation.
    fn hold_session(&self, held: bool);
    /// The enclave was booted anew under a hibernation image: the session
    /// is gone. Returns whether the device should be probed again.
    fn session_lost(&self) -> bool;
    /// Whether this kernel registered with the enclave.
    fn attached(&self) -> bool;
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
    fn hold_session(&self, held: bool) {
        self.hibernation_hold(held);
    }
    fn session_lost(&self) -> bool {
        self.lose_session()
    }
    fn attached(&self) -> bool {
        SepData::attached(self)
    }
}

/// Takes or releases the holds for one sleep event. Returns 1 when the
/// device must be probed again, or for `EVENT_RESTORE` when the restore must
/// be refused; 0 otherwise.
fn sleep_event(sep: &impl SleepHolds, event: c_int) -> c_int {
    match event {
        EVENT_SLEEP => {
            sep.hold_entropy();
            sep.hold_captures();
        }
        EVENT_HIBERNATE => {
            sep.hold_entropy();
            sep.hold_captures();
            sep.hold_session(true);
        }
        EVENT_WAKE => {
            sep.release_captures();
            sep.release_entropy();
        }
        // This kernel is about to load a hibernation image: refused (1) if
        // it registered with the enclave, see `pm_shim.c`.
        EVENT_RESTORE => {
            if sep.attached() {
                return 1;
            }
            sep.hold_entropy();
            sep.hold_captures();
            sep.hold_session(true);
        }
        EVENT_HIBERNATE_WARM => {
            sep.hold_session(false);
            sep.release_captures();
            sep.release_entropy();
        }
        // The holds stay: the removal ahead of the new probe releases them
        // into a session that refuses everything.
        EVENT_HIBERNATE_COLD => return sep.session_lost() as c_int,
        _ => {}
    }
    0
}

/// # Safety
/// Called by `pm_shim.c` from the PM notifier chain, in process context.
unsafe extern "C" fn pm_event(event: c_int) -> c_int {
    let ptr = SEP.load(Ordering::Acquire);
    if ptr.is_null() {
        return 0;
    }
    // SAFETY: produced by `into_foreign` in `register`, and reclaimed by
    // `unregister` only after `sep_pm_unregister` has waited out any running
    // notifier call, so it is live for this borrow.
    let sep = unsafe { <Arc<SepData> as ForeignOwnable>::borrow(ptr) };
    sleep_event(&*sep, event)
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
    let ptr = SEP.load(Ordering::Acquire);
    if ptr.is_null() {
        return;
    }
    // SAFETY: as in `pm_event`.
    let sep = unsafe { <Arc<SepData> as ForeignOwnable>::borrow(ptr) };
    if sep.sleep_exit() {
        // The enclave slept and is back: report the accessory service online
        // again, as macOS does right after the wake.
        SepData::queue_stac_online(sep.into());
    }
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
    // `dev` is the bound SEP device: the shim drops it in `sep_pm_unregister`
    // and takes its own reference for a reprobe.
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
    const HOLD_SESSION: u8 = 5;
    const RELEASE_SESSION: u8 = 6;
    const LOSE_SESSION: u8 = 7;

    struct Calls {
        made: RefCell<[u8; 8]>,
        count: Cell<usize>,
        lose: Cell<bool>,
        attached: Cell<bool>,
    }

    impl Calls {
        fn new() -> Self {
            Calls {
                made: RefCell::new([0; 8]),
                count: Cell::new(0),
                lose: Cell::new(true),
                attached: Cell::new(false),
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
        fn hold_session(&self, held: bool) {
            self.call(if held { HOLD_SESSION } else { RELEASE_SESSION });
        }
        fn session_lost(&self) -> bool {
            self.call(LOSE_SESSION);
            self.lose.get()
        }
        fn attached(&self) -> bool {
            self.attached.get()
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
        assert_eq!(sleep_event(&calls, EVENT_SLEEP), 0);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, 0, 0, 0, 0, 0, 0]
        );
    }

    #[test]
    fn sleep_holds_stay_until_the_post_event() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, EVENT_SLEEP), 0);
        assert_eq!(calls.count.get(), 2);
        assert_eq!(sleep_event(&calls, EVENT_WAKE), 0);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, RELEASE_CAPTURES, RELEASE_ENTROPY, 0, 0, 0, 0]
        );
    }

    #[test]
    fn waking_releases_captures_then_entropy() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, EVENT_WAKE), 0);
        assert_eq!(
            calls.made(),
            [RELEASE_CAPTURES, RELEASE_ENTROPY, 0, 0, 0, 0, 0, 0]
        );
    }

    #[test]
    fn a_hibernation_holds_the_session_last() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, EVENT_HIBERNATE), 0);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, HOLD_SESSION, 0, 0, 0, 0, 0]
        );
    }

    #[test]
    fn a_warm_end_of_a_hibernation_lifts_the_session_hold_first() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, EVENT_HIBERNATE), 0);
        assert_eq!(sleep_event(&calls, EVENT_HIBERNATE_WARM), 0);
        assert_eq!(
            calls.made(),
            [
                HOLD_ENTROPY,
                HOLD_CAPTURES,
                HOLD_SESSION,
                RELEASE_SESSION,
                RELEASE_CAPTURES,
                RELEASE_ENTROPY,
                0,
                0
            ]
        );
    }

    #[test]
    fn a_cold_resume_keeps_the_holds_and_asks_for_a_probe() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, EVENT_HIBERNATE), 0);
        assert_eq!(sleep_event(&calls, EVENT_HIBERNATE_COLD), 1);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, HOLD_SESSION, LOSE_SESSION, 0, 0, 0, 0]
        );
    }

    #[test]
    fn a_second_cold_resume_asks_for_no_second_probe() {
        let calls = Calls::new();
        calls.lose.set(false);
        assert_eq!(sleep_event(&calls, EVENT_HIBERNATE_COLD), 0);
    }

    #[test]
    fn a_restore_over_an_attached_enclave_is_refused_without_holds() {
        let calls = Calls::new();
        calls.attached.set(true);
        assert_eq!(sleep_event(&calls, EVENT_RESTORE), 1);
        assert_eq!(calls.count.get(), 0);
    }

    #[test]
    fn a_restore_without_a_session_holds_like_a_hibernation() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, EVENT_RESTORE), 0);
        assert_eq!(
            calls.made(),
            [HOLD_ENTROPY, HOLD_CAPTURES, HOLD_SESSION, 0, 0, 0, 0, 0]
        );
    }

    #[test]
    fn an_unknown_event_changes_nothing() {
        let calls = Calls::new();
        assert_eq!(sleep_event(&calls, 99), 0);
        assert_eq!(calls.count.get(), 0);
    }
}
