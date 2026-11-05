// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Work-based firmware performance effort. Idle power gating is managed separately by the
//! device-control ring; this input never keeps an otherwise idle GPU powered.

pub(super) const FULL_EFFORT: u32 = 0x10000;
const IDLE_HOLD_NS: u64 = 16_000_000;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) enum IdleAction {
    None,
    Clear,
    Recheck(u64),
}

pub(super) struct Effort {
    streaming: bool,
    idle_since: Option<u64>,
    applied: u32,
}

impl Effort {
    pub(super) const fn new() -> Self {
        Self {
            streaming: false,
            idle_since: None,
            applied: 0,
        }
    }

    pub(super) fn submit(&mut self) {
        self.streaming = true;
        self.idle_since = None;
    }

    pub(super) fn applied(&self) -> u32 {
        self.applied
    }
    pub(super) fn did_apply(&mut self, value: u32) {
        self.applied = value;
    }

    /// Quarantined jobs do not count as work in flight; they retain mappings but cannot justify
    /// retaining the effort input. New submissions restart the continuous-idle interval.
    pub(super) fn idle(&mut self, now_ns: u64, in_flight: bool) -> IdleAction {
        if !self.streaming {
            return IdleAction::None;
        }
        if in_flight {
            self.idle_since = None;
            return IdleAction::None;
        }
        let since = *self.idle_since.get_or_insert(now_ns.max(1));
        let idle = now_ns.saturating_sub(since);
        if idle >= IDLE_HOLD_NS {
            self.streaming = false;
            self.idle_since = None;
            IdleAction::Clear
        } else {
            IdleAction::Recheck(IDLE_HOLD_NS - idle)
        }
    }
}

/// Single-producer control path under the device mutex. Event draining must not publish another
/// control between `set_idle` and the exact three-counter retirement witness.
pub(super) trait Control {
    fn crashed(&self) -> bool;
    fn powered(&self) -> bool;
    fn counters(&self) -> kernel::error::Result<[u32; 3]>;
    fn set_idle(&mut self, enabled: bool) -> kernel::error::Result<u32>;
    fn wait_tick(&self);
    fn registration_release_failed(&mut self) {}
    fn registration_release_succeeded(&mut self) {}
}

fn wait<C: Control>(control: &C, target: u32, require_powered: bool) -> kernel::error::Result {
    use kernel::prelude::*;
    const RETIRE_POLLS: usize = 20;
    const POWER_POLLS: usize = 250;
    let mut retired = false;
    for elapsed in 0..RETIRE_POLLS + POWER_POLLS {
        if control.crashed() {
            return Err(EIO);
        }
        if !retired {
            retired = control.counters()? == [target; 3];
            if !retired && elapsed + 1 >= RETIRE_POLLS {
                break;
            }
        }
        if retired && (!require_powered || control.powered()) {
            return Ok(());
        }
        control.wait_tick();
    }
    Err(ETIMEDOUT)
}

/// Disable idle descent through fresh pair registration, including a post-wake reassertion.
pub(super) fn acquire<C: Control>(control: &mut C) -> kernel::error::Result {
    use kernel::prelude::*;
    let result = (|| {
        for _ in 0..3 {
            let powered = control.powered();
            let target = control.set_idle(false)?;
            wait(control, target, true)?;
            if !powered {
                let target = control.set_idle(false)?;
                wait(control, target, true)?;
            }
            if control.powered() {
                return Ok(());
            }
        }
        Err(EAGAIN)
    })();
    if result.is_err() && release(control).is_err() {
        control.registration_release_failed();
    }
    result
}

pub(super) fn release<C: Control>(control: &mut C) -> kernel::error::Result {
    let target = match control.set_idle(true) {
        Ok(target) => target,
        Err(error) => {
            control.registration_release_failed();
            return Err(error);
        }
    };
    let result = wait(control, target, false);
    if result.is_ok() {
        control.registration_release_succeeded();
    } else {
        control.registration_release_failed();
    }
    result
}
