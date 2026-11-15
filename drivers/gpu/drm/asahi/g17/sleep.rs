// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! System sleep: the firmware goes idle, its domain goes off, and it restarts warm.
//!
//! The firmware isn't initialized again. It keeps its rings and state in DRAM, so a restart is
//! the RTKit handshake and the end of the power-on transition. Publication stays closed from
//! before the idle until after the restart, and nothing is in flight across it: work that doesn't
//! finish in time refuses the sleep.
//!
//! Order, on both instances, is what macOS does on the chip whose [`super::Config`] sets
//! `sleep_restart`. Other chips refuse.
//!
//! Only a real wake restarts the firmware. A sleep refused after the firmware stopped leaves it
//! stopped and publication closed until the next real sleep, with the domain on so that everything
//! behind it stays reachable. That sleep then only has the domain left to lower.
//!
//! `.prepare` only drains and checks for quiet, and the sleep's commit makes the whole stop in C,
//! with interrupts off and every refusal behind it. A sleep refused before then finds the firmware
//! never stopped. Where the commit can't take the stop, `.prepare` refuses the sleep: a stop of its
//! own would leave the firmware stopped after any later refusal, until the next real sleep.

use super::{Firmware, Role, Shared};
use core::sync::atomic::Ordering;
use kernel::{
    bindings,
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta},
    workqueue::{DelayedWork, Work},
};

/// Time work in flight gets to finish before the sleep is refused.
const DRAIN_NS: u64 = 3_000_000_000;
const DRAIN_TICK_MS: i64 = 2;

unsafe extern "C" {
    fn asahi_g17_pmp_request(dev: *mut core::ffi::c_void, enable: bool) -> core::ffi::c_int;
    fn asahi_g17_power_auto_off(dev: *mut core::ffi::c_void, saved: *mut u32) -> core::ffi::c_int;
    fn asahi_g17_power_lower(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
    fn asahi_g17_power_raise(dev: *mut core::ffi::c_void, saved: u32) -> core::ffi::c_int;
    fn asahi_g17_power_auto_on(dev: *mut core::ffi::c_void, saved: u32) -> core::ffi::c_int;
    fn asahi_g17_commit_prepare(
        dev: *mut core::ffi::c_void,
        primary_rtk: *mut bindings::apple_rtkit,
        primary_mbox: *mut core::ffi::c_void,
        secondary_rtk: *mut bindings::apple_rtkit,
        secondary_mbox: *mut core::ffi::c_void,
        bridge: bool,
    ) -> *mut core::ffi::c_void;
    fn asahi_g17_commit_finish(commit: *mut core::ffi::c_void, saved: *mut u32) -> bool;
}

/// What `.complete` does after a sleep whose stop was left to the commit.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum AfterCommit {
    /// Refused before the commit: the firmware never stopped, so only publication reopens.
    Running,
    /// Stopped at the commit, or its power lost anyway: the firmware has to restart.
    Stopped,
}

fn after_commit(stopped: bool, real: bool) -> AfterCommit {
    if stopped || real {
        AfterCommit::Stopped
    } else {
        AfterCommit::Running
    }
}

/// What restarting the firmware and lowering or raising its domain touches, in the order [`up`]
/// uses it. The stop itself is the sleep's commit's.
pub(super) trait Hardware {
    fn has_secondary(&self) -> bool;
    /// The PMP's request for the GPU. It may use a device it's told is in use, so the request is
    /// only ever set with the domain on.
    fn request(&mut self, enable: bool);
    /// Core gating as it is while the firmware runs, or held for a domain about to go off.
    fn gating(&mut self, enable: bool) -> Result;
    /// The GPU-to-fabric bridge.
    fn bridge(&mut self, enable: bool) -> Result;
    /// Automatic power management off, keeping the word for the way back.
    fn auto_off(&mut self) -> Result;
    /// The domain off, waiting until it has settled.
    fn lower(&mut self) -> Result;
    /// The domain on, waiting until it is.
    fn raise(&mut self) -> Result;
    /// Automatic power management as it was.
    fn auto_on(&mut self) -> Result;
    /// Run bit on, RTKit handshake to AP on.
    fn restart(&mut self, role: Role) -> Result;
    /// Ends the power-on transition, on every instance.
    fn power_on_done(&mut self) -> Result;
}

/// Lowers the domain, with the firmware stopped.
fn lower_domain<H: Hardware>(hw: &mut H) {
    let _ = hw.auto_off();
    let _ = hw.bridge(false);
    let _ = hw.lower();
}

/// Brings the domain back for firmware that stays stopped after a refused sleep.
fn hold<H: Hardware>(hw: &mut H) -> Result {
    hw.raise()?;
    hw.auto_on()
}

/// Restarts the firmware. Any error loses the device: there is no other way back.
pub(super) fn up<H: Hardware>(hw: &mut H) -> Result {
    hw.raise()?;
    hw.bridge(true)?;
    hw.auto_on()?;
    hw.request(true);
    hw.gating(true)?;
    if hw.has_secondary() {
        hw.restart(Role::Secondary)?;
    }
    hw.restart(Role::Primary)?;
    hw.power_on_done()
}

impl Hardware for Firmware {
    fn has_secondary(&self) -> bool {
        self.secondary.is_some()
    }

    fn request(&mut self, enable: bool) {
        self.primary.state.shared.pmp_request(enable);
    }

    fn gating(&mut self, enable: bool) -> Result {
        self.bridge.set_gating(enable)
    }

    fn bridge(&mut self, enable: bool) -> Result {
        if let Some([first, second, gating]) = self.bridge.words() {
            dev_dbg!(
                self.primary.state.shared.dev.as_ref(),
                "GPU before bridge {}: {:#x} {:#x} gating {:#x}\n",
                if enable { "on" } else { "off" },
                first,
                second,
                gating
            );
        }
        self.bridge.set(enable)
    }

    fn auto_off(&mut self) -> Result {
        self.primary.state.shared.power_auto_off()
    }

    fn lower(&mut self) -> Result {
        self.primary.state.shared.power_lower()
    }

    fn raise(&mut self) -> Result {
        let result = self.primary.state.shared.power_raise();
        if result.is_err() {
            // The registers aren't reachable, so teardown can't clear the bridge.
            self.bridge.forget();
        }
        result
    }

    fn auto_on(&mut self) -> Result {
        self.primary.state.shared.power_auto_on()
    }

    fn restart(&mut self, role: Role) -> Result {
        let shared = self.primary.state.shared.clone();
        let coprocessor = self.coprocessor(role)?;
        let result = coprocessor.restart();
        if let (Err(_), Ok((control, status))) = (&result, coprocessor.cpu.state()) {
            dev_err!(
                shared.dev.as_ref(),
                "GPU firmware {:?}: control {:#x} status {:#x}\n",
                role,
                control,
                status
            );
        }
        result
    }

    fn power_on_done(&mut self) -> Result {
        if !self.variant.power_on_done {
            return Ok(());
        }
        self.primary.notify(super::MSG_POWER_ON_DONE)?;
        if let Some(secondary) = self.secondary.as_ref() {
            secondary.notify(super::MSG_POWER_ON_DONE)?;
        }
        Ok(())
    }
}

impl Firmware {
    /// Hands the sleep's commit what its stop needs. Nothing stops here.
    fn leave_for_commit(&mut self) -> Result<*mut core::ffi::c_void> {
        let shared = self.primary.state.shared.clone();
        let primary_rtk = self.primary.rtkit.with(|rtkit| rtkit.as_raw());
        let primary_mbox = self.primary.cpu.as_raw().cast();
        let (secondary_rtk, secondary_mbox) = match self.secondary.as_ref() {
            Some(secondary) => (
                secondary.rtkit.with(|rtkit| rtkit.as_raw()),
                secondary.cpu.as_raw().cast(),
            ),
            None => (core::ptr::null_mut(), core::ptr::null_mut()),
        };
        // SAFETY: `dev` is the live GPU device. The RTKit and mailbox pointers stay valid while
        // the firmware exists, and `.complete` frees the result before anything drops them.
        let commit = unsafe {
            asahi_g17_commit_prepare(
                shared.dev.as_raw().cast(),
                primary_rtk,
                primary_mbox,
                secondary_rtk,
                secondary_mbox,
                self.bridge.enabled,
            )
        };
        kernel::error::from_err_ptr(commit)
    }

    /// The books for both instances, which the commit stopped.
    fn stopped_at_commit(&mut self) {
        if let Some(secondary) = self.secondary.as_mut() {
            secondary.stopped_at_commit();
        }
        self.primary.stopped_at_commit();
    }

    fn coprocessor(&mut self, role: Role) -> Result<&mut super::Coprocessor> {
        match role {
            Role::Primary => Ok(&mut self.primary),
            Role::Secondary => self.secondary.as_mut().ok_or(ENODEV),
        }
    }
}

impl Shared {
    pub(super) fn sleeping(&self) -> bool {
        self.sleeping.load(Ordering::Acquire)
    }

    /// A PMP that doesn't answer in time only gets a warning: nothing better is left to do here.
    fn pmp_request(&self, enable: bool) {
        // SAFETY: `dev` is the live GPU device.
        let result = kernel::error::to_result(unsafe {
            asahi_g17_pmp_request(self.dev.as_raw().cast(), enable)
        });
        if let Err(error) = result {
            dev_warn!(self.dev.as_ref(), "PMP request for the GPU, {}: {:?}\n", enable, error);
        }
    }

    fn power_auto_off(&self) -> Result {
        let mut saved = 0;
        // SAFETY: `dev` is the live GPU device and `saved` is writable.
        let result = kernel::error::to_result(unsafe {
            asahi_g17_power_auto_off(self.dev.as_raw().cast(), &mut saved)
        });
        self.sleep_ps.store(saved, Ordering::Relaxed);
        result
    }

    fn power_lower(&self) -> Result {
        // SAFETY: `dev` is the live GPU device.
        let result =
            kernel::error::to_result(unsafe { asahi_g17_power_lower(self.dev.as_raw().cast()) });
        if let Err(error) = result {
            dev_warn!(self.dev.as_ref(), "GPU domain slow to go off: {:?}\n", error);
        }
        result
    }

    fn power_raise(&self) -> Result {
        let saved = self.sleep_ps.load(Ordering::Relaxed);
        // SAFETY: `dev` is the live GPU device.
        kernel::error::to_result(unsafe { asahi_g17_power_raise(self.dev.as_raw().cast(), saved) })
    }

    fn power_auto_on(&self) -> Result {
        let saved = self.sleep_ps.load(Ordering::Relaxed);
        // SAFETY: `dev` is the live GPU device.
        kernel::error::to_result(unsafe {
            asahi_g17_power_auto_on(self.dev.as_raw().cast(), saved)
        })
    }

    fn cancel_timer<const ID: u64>(self: &Arc<Self>, timer: &DelayedWork<Self, ID>) {
        // SAFETY: As in `disable_timer`: a pending callback owned one Arc that it can no longer
        // reclaim. Future enqueues stay possible and run into the sleeping check.
        let cancelled = unsafe {
            let work = Work::raw_get(DelayedWork::raw_as_work(timer));
            let delayed = kernel::container_of!(work, bindings::delayed_work, work);
            bindings::cancel_delayed_work_sync(delayed)
        };
        if cancelled {
            // SAFETY: See above.
            drop(unsafe { Arc::from_raw(Arc::as_ptr(self)) });
        }
    }

    /// With the sleeping flag set, joins whatever may be reading firmware state. The growth work
    /// isn't joined: it may be waiting on the closed gate, and can't publish while it is closed.
    fn join_event_workers(self: &Arc<Self>) {
        self.cancel_timer(&self.poll);
        self.cancel_timer(&self.idle);
        // SAFETY: An initialized, pinned work field of this Shared.
        unsafe { bindings::flush_work(Work::raw_get(&self.work)) };
    }

    /// Waits for what's been published to retire. The event worker keeps running meanwhile, and
    /// its own idle check takes the power and effort holds down.
    fn wait_quiet(self: &Arc<Self>, deadline: u64) -> Result {
        loop {
            if self.crashed.load(Ordering::Acquire) {
                return Err(ENODEV);
            }
            let now = super::now_ns();
            {
                let mut state = self.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                // Nothing else retries this with no render publication coming.
                if firmware.registration_power_release_pending {
                    let _ = firmware.release_registration_power();
                }
                if firmware.sleep_quiet(now)? {
                    return Ok(());
                }
            }
            if now >= deadline {
                return Err(EBUSY);
            }
            self.queue_events();
            fsleep(Delta::from_millis(DRAIN_TICK_MS));
        }
    }

    /// Undoes everything short of a stopped firmware. A builder that never finishes would keep
    /// publication closed for good, and that is a lost device.
    fn sleep_cancel(self: &Arc<Self>) {
        self.sleeping.store(false, Ordering::Release);
        self.sleep_gate.store(false, Ordering::Release);
        let drained = self
            .preparations
            .wait_drained_until(super::now_ns().saturating_add(DRAIN_NS));
        if (drained.is_err() || !self.preparations.reopen())
            && !self.crashed.load(Ordering::Acquire)
        {
            self.lose_device_quietly();
        }
        self.queue_events();
    }

    /// Keeps the firmware stopped after a sleep that never reached the hardware. False if there's
    /// no firmware to keep, or no domain to keep it in.
    fn sleep_hold(self: &Arc<Self>) -> bool {
        if self.crashed.load(Ordering::Acquire) {
            return false;
        }
        let mut state = self.state.lock();
        let Some(firmware) = (*state).as_deref_mut() else {
            return false;
        };
        if hold(firmware).is_err() {
            return false;
        }
        dev_warn!(
            self.dev.as_ref(),
            "sleep refused: GPU firmware stays stopped until a real sleep\n"
        );
        true
    }

    /// Leaves the stop to the commit, or refuses the sleep if the commit can't take it: a stop
    /// here would leave the firmware stopped after any later refusal, until the next real sleep.
    fn sleep_leave_for_commit(&self, firmware: &mut Firmware) -> Result {
        match firmware.leave_for_commit() {
            Ok(commit) => {
                self.sleep_commit.store(commit, Ordering::Release);
                dev_dbg!(self.dev.as_ref(), "GPU firmware left running for the sleep's commit\n");
                Ok(())
            }
            Err(error) => {
                dev_err!(
                    self.dev.as_ref(),
                    "no GPU stop at the commit ({:?}): refusing the sleep\n",
                    error
                );
                Err(EBUSY)
            }
        }
    }

    /// Whether the commit stopped the firmware, if the stop was left to it.
    fn sleep_commit_finish(&self) -> Option<bool> {
        let commit = self.sleep_commit.swap(core::ptr::null_mut(), Ordering::AcqRel);
        if commit.is_null() {
            return None;
        }
        let mut saved = self.sleep_ps.load(Ordering::Relaxed);
        // SAFETY: `commit` came from `asahi_g17_commit_prepare` and is finished only once.
        let stopped = unsafe { asahi_g17_commit_finish(commit, &mut saved) };
        self.sleep_ps.store(saved, Ordering::Relaxed);
        Some(stopped)
    }

    /// After the commit stopped the firmware: the books it left for each stopped instance.
    fn sleep_settle_commit(&self) {
        let mut state = self.state.lock();
        if let Some(firmware) = (*state).as_deref_mut() {
            firmware.stopped_at_commit();
        }
    }

    /// Closes publication, waits for quiet, and leaves the stop to the sleep's commit. A refusal
    /// changes nothing.
    fn sleep_down(self: &Arc<Self>) -> Result {
        if self.sleeping() {
            let mut state = self.state.lock();
            if let Some(firmware) = (*state).as_deref_mut() {
                lower_domain(firmware);
            }
            return Ok(());
        }
        let deadline = super::now_ns().saturating_add(DRAIN_NS);
        self.sleep_gate.store(true, Ordering::Release);
        self.preparations.close();
        let result = self.preparations.wait_drained_until(deadline).map_err(|_| EBUSY)
            .and_then(|_| self.wait_quiet(deadline))
            .and_then(|_| {
                self.sleeping.store(true, Ordering::Release);
                self.join_event_workers();
                let mut state = self.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !firmware.sleep_quiet(super::now_ns())? {
                    return Err(EBUSY);
                }
                self.sleep_leave_for_commit(firmware)
            });
        match result {
            Ok(()) => Ok(()),
            Err(error) if error == ENODEV || self.crashed.load(Ordering::Acquire) => {
                // Nothing to save: let the worker finish detaching what's left, and then the
                // domain only has to be off for the sleep.
                self.sleeping.store(false, Ordering::Release);
                self.queue_events();
                // SAFETY: An initialized, pinned work field of this Shared.
                unsafe { bindings::flush_work(Work::raw_get(&self.work)) };
                self.sleeping.store(true, Ordering::Release);
                self.join_event_workers();
                self.pmp_request(false);
                let _ = self.power_auto_off();
                let _ = self.power_lower();
                Ok(())
            }
            Err(error) => {
                self.sleep_cancel();
                Err(error)
            }
        }
    }

    /// Restarts the firmware and reopens publication. A failure loses the device.
    fn sleep_up(self: &Arc<Self>) -> Result {
        let started = super::now_ns();
        let result = {
            let mut state = self.state.lock();
            match (*state).as_deref_mut() {
                Some(firmware) if !self.crashed.load(Ordering::Acquire) => {
                    let result = up(firmware);
                    if result.is_ok() {
                        // The cores lost power, so the next command on each pair installs it again.
                        firmware.queues.forget_render_registrations();
                        firmware.rings.invalidate_statistics();
                        firmware.port.statistics_epoch_bump();
                    }
                    result
                }
                _ => self
                    .power_raise()
                    .and_then(|_| self.power_auto_on())
                    .inspect(|_| self.pmp_request(true)),
            }
        };
        self.sleeping.store(false, Ordering::Release);
        self.sleep_gate.store(false, Ordering::Release);
        match result {
            Ok(()) => {
                if !self.crashed.load(Ordering::Acquire) {
                    dev_dbg!(
                        self.dev.as_ref(),
                        "GPU firmware back in {} us\n",
                        super::now_ns().saturating_sub(started) / 1000
                    );
                }
                self.preparations.reopen();
                self.queue_events();
                Ok(())
            }
            Err(error) => {
                dev_err!(self.dev.as_ref(), "GPU firmware did not come back: {:?}\n", error);
                self.lose_device_quietly();
                Err(error)
            }
        }
    }
}

impl super::Gpu {
    /// Called before any device suspends. Only a sleep that powers the SoC down gets here.
    pub(crate) fn sleep_prepare(&self) -> Result {
        if !self.cfg.sleep_restart {
            dev_warn!(
                self.shared.dev.as_ref(),
                "no firmware restart on this chip, refusing sleep\n"
            );
            return Err(EBUSY);
        }
        let result = self.shared.sleep_down();
        if let Err(error) = result {
            dev_warn!(self.shared.dev.as_ref(), "GPU refused sleep: {:?}\n", error);
        }
        result
    }

    /// Called after every device resumed, whether or not the sleep reached the hardware.
    pub(crate) fn sleep_complete(&self) {
        if !self.shared.sleeping() {
            return;
        }
        // SAFETY: Reads a global flag word.
        let real = unsafe { bindings::pm_resume_via_firmware() };
        if let Some(stopped) = self.shared.sleep_commit_finish() {
            if after_commit(stopped, real) == AfterCommit::Running {
                dev_info!(
                    self.shared.dev.as_ref(),
                    "sleep refused before the commit: GPU firmware never stopped\n"
                );
                self.shared.sleep_cancel();
                return;
            }
            self.shared.sleep_settle_commit();
        }
        if !real && self.shared.sleep_hold() {
            return;
        }
        let _ = self.shared.sleep_up();
    }
}

#[kernel::macros::kunit_tests(asahi_g17_sleep)]
mod tests {
    use super::*;

    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    enum Call {
        Request(bool),
        Gating(bool),
        Bridge(bool),
        AutoOff,
        Lower,
        Raise,
        AutoOn,
        Restart(Role),
        Done,
    }

    struct Fake {
        secondary: bool,
        fail: Option<Call>,
        calls: [Option<Call>; 14],
        count: usize,
    }

    impl Fake {
        fn new(secondary: bool, fail: Option<Call>) -> Self {
            Self { secondary, fail, calls: [None; 14], count: 0 }
        }

        fn note(&mut self, call: Call) -> Result {
            self.calls[self.count] = Some(call);
            self.count += 1;
            if self.fail == Some(call) { Err(EIO) } else { Ok(()) }
        }

        fn called(&self, expected: &[Call]) -> bool {
            self.count == expected.len()
                && expected.iter().zip(self.calls.iter()).all(|(a, b)| Some(*a) == *b)
        }
    }

    impl Hardware for Fake {
        fn has_secondary(&self) -> bool { self.secondary }
        fn request(&mut self, enable: bool) { let _ = self.note(Call::Request(enable)); }
        fn gating(&mut self, enable: bool) -> Result { self.note(Call::Gating(enable)) }
        fn bridge(&mut self, enable: bool) -> Result { self.note(Call::Bridge(enable)) }
        fn auto_off(&mut self) -> Result { self.note(Call::AutoOff) }
        fn lower(&mut self) -> Result { self.note(Call::Lower) }
        fn raise(&mut self) -> Result { self.note(Call::Raise) }
        fn auto_on(&mut self) -> Result { self.note(Call::AutoOn) }
        fn restart(&mut self, role: Role) -> Result { self.note(Call::Restart(role)) }
        fn power_on_done(&mut self) -> Result { self.note(Call::Done) }
    }

    use Role::{Primary as P, Secondary as S};

    #[test]
    fn up_restarts_the_secondary_first_and_ends_the_transition_last() {
        let mut hw = Fake::new(true, None);
        assert_eq!(up(&mut hw), Ok(()));
        assert!(hw.called(&[
            Call::Raise,
            Call::Bridge(true),
            Call::AutoOn,
            Call::Request(true),
            Call::Gating(true),
            Call::Restart(S),
            Call::Restart(P),
            Call::Done,
        ]));
    }

    #[test]
    fn a_single_instance_never_names_a_secondary() {
        let mut hw = Fake::new(false, None);
        assert_eq!(up(&mut hw), Ok(()));
        assert!(hw.called(&[
            Call::Raise,
            Call::Bridge(true),
            Call::AutoOn,
            Call::Request(true),
            Call::Gating(true),
            Call::Restart(P),
            Call::Done,
        ]));
    }

    #[test]
    fn a_refused_sleep_only_brings_the_domain_back() {
        let mut hw = Fake::new(true, None);
        assert_eq!(hold(&mut hw), Ok(()));
        assert!(hw.called(&[Call::Raise, Call::AutoOn]));
        let mut hw = Fake::new(true, Some(Call::Raise));
        assert_eq!(hold(&mut hw), Err(EIO));
        assert!(hw.called(&[Call::Raise]));
    }

    #[test]
    fn the_sleep_after_a_refusal_only_lowers_the_domain() {
        let mut hw = Fake::new(true, None);
        lower_domain(&mut hw);
        assert!(hw.called(&[Call::AutoOff, Call::Bridge(false), Call::Lower]));
    }

    #[test]
    fn up_stops_at_the_first_failure() {
        let mut hw = Fake::new(true, Some(Call::Restart(S)));
        assert_eq!(up(&mut hw), Err(EIO));
        assert!(hw.called(&[
            Call::Raise,
            Call::Bridge(true),
            Call::AutoOn,
            Call::Request(true),
            Call::Gating(true),
            Call::Restart(S),
        ]));
        // No domain, so no bridge and no request either.
        let mut hw = Fake::new(true, Some(Call::Raise));
        assert_eq!(up(&mut hw), Err(EIO));
        assert!(hw.called(&[Call::Raise]));
        // No bridge, so the PMP isn't told about the GPU.
        let mut hw = Fake::new(true, Some(Call::Bridge(true)));
        assert_eq!(up(&mut hw), Err(EIO));
        assert!(hw.called(&[Call::Raise, Call::Bridge(true)]));
    }

    #[test]
    fn a_stop_left_to_the_commit_is_undone_only_if_it_never_ran() {
        assert_eq!(after_commit(false, false), AfterCommit::Running);
        assert_eq!(after_commit(true, false), AfterCommit::Stopped);
        assert_eq!(after_commit(true, true), AfterCommit::Stopped);
        // No stop, but the power went anyway: the firmware has to restart.
        assert_eq!(after_commit(false, true), AfterCommit::Stopped);
    }

}
