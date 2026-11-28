// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Work-based firmware performance effort. Idle power gating is managed separately by the
//! device-control ring; this input never keeps an otherwise idle GPU powered.

use core::sync::atomic::AtomicU32;

pub(super) const FULL_EFFORT: u32 = 0x10000;
pub(super) const IDLE_HOLD_NS: u64 = 16_000_000;

/// The firmware's idle power-off delay as measured on T6040/25G83: the cores power off 2 ms after
/// the last job. Not measured on other firmware; at or below it the hold stays off.
pub(crate) const FIRMWARE_IDLE_OFF_MS: u32 = 2;

/// Idle policy initialized from the existing DT `apple,idle-off-delay-ms` property.
/// The existing `idle_off_delay_ms` sysfs attribute may override it. Above
/// [`FIRMWARE_IDLE_OFF_MS`] the driver keeps the GPU cores powered until it has been idle
/// this long ([`Hold`]).
pub(crate) static IDLE_HOLD_MS: AtomicU32 = AtomicU32::new(FIRMWARE_IDLE_OFF_MS);

/// Use the same bounds as the existing idle-delay attribute. Missing DT policy
/// preserves the native delay; malformed or out-of-range values fail probe.
pub(super) fn idle_delay(requested: Option<u32>) -> kernel::error::Result<u32> {
    let delay = requested.unwrap_or(FIRMWARE_IDLE_OFF_MS);
    if !(FIRMWARE_IDLE_OFF_MS..=1000).contains(&delay) {
        return Err(kernel::error::code::EINVAL);
    }
    Ok(delay)
}

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
    /// The firmware's effort word is known to hold `applied`: the host wrote it, or read it.
    confirmed: bool,
    submissions: u64,
    /// A host thread waits, or waited within [`IDLE_HOLD_NS`], for submitted work
    /// ([`Self::note_waiters`]).
    waited: bool,
    /// The waits begun when the waiters were last noted.
    seen_waits: u64,
    /// When a waiter was last seen.
    last_wait_ns: u64,
    /// A host thread waits, or waited within [`IDLE_HOLD_NS`], for a pure-compute submission
    /// ([`Self::note_compute_waiters`]).
    compute_waited: bool,
    /// Pure-compute waits begun when compute waiters were last noted.
    seen_compute_waits: u64,
    /// When a pure-compute waiter was last seen.
    last_compute_wait_ns: u64,
    /// The GPU has run longer than [`IDLE_HOLD_NS`] without an idle gap ([`BusyRun`]).
    bound: bool,
}

impl Effort {
    pub(super) const fn new() -> Self {
        Self {
            streaming: false,
            idle_since: None,
            applied: 0,
            confirmed: false,
            submissions: 0,
            waited: false,
            seen_waits: 0,
            last_wait_ns: 0,
            compute_waited: false,
            seen_compute_waits: 0,
            last_compute_wait_ns: 0,
            bound: false,
        }
    }

    pub(super) fn streaming(&self) -> bool {
        self.streaming
    }

    /// Whether a host thread waits for the work ([`Self::note_waiters`]).
    pub(super) fn waited(&self) -> bool {
        self.waited
    }

    /// Notes the waiters on submitted work at `now_ns` ([`super::fence::waiters`]): a thread
    /// waits now or began a wait since the last note. The note holds for [`IDLE_HOLD_NS`] after
    /// the last waiter, across a submit-and-wait loop's turns between its waits. Returns whether
    /// that changed.
    pub(super) fn note_waiters(&mut self, now_ns: u64, (waiters, waits): (u32, u64)) -> bool {
        if waiters > 0 || waits != self.seen_waits {
            self.last_wait_ns = now_ns.max(1);
        }
        self.seen_waits = waits;
        let waited = self.last_wait_ns != 0 && now_ns.saturating_sub(self.last_wait_ns) < IDLE_HOLD_NS;
        let changed = waited != self.waited;
        self.waited = waited;
        changed
    }

    /// Whether a host thread waits for a pure-compute submission ([`Self::note_compute_waiters`]).
    pub(super) fn compute_waited(&self) -> bool {
        self.compute_waited
    }

    /// Notes host waiters on pure-compute submissions at `now_ns`
    /// ([`super::fence::compute_waiters`]). Holds for [`IDLE_HOLD_NS`] after the last compute
    /// wait so depth-1 compute loops stay at full effort across host turnarounds and resume at
    /// full effort immediately after CPU gaps without affecting paced render compositors.
    pub(super) fn note_compute_waiters(
        &mut self,
        now_ns: u64,
        (waiters, waits): (u32, u64),
    ) -> bool {
        if waiters > 0 || waits != self.seen_compute_waits {
            self.last_compute_wait_ns = now_ns.max(1);
        }
        self.seen_compute_waits = waits;
        let compute_waited = self.last_compute_wait_ns != 0
            && now_ns.saturating_sub(self.last_compute_wait_ns) < IDLE_HOLD_NS;
        let changed = compute_waited != self.compute_waited;
        self.compute_waited = compute_waited;
        changed
    }

    /// Whether [`Self::idle`] at `now_ns` with `in_flight` will clear the effort state.
    pub(super) fn will_clear(&self, now_ns: u64, in_flight: bool) -> bool {
        self.streaming
            && !in_flight
            && self
                .idle_since
                .is_some_and(|since| now_ns.saturating_sub(since) >= IDLE_HOLD_NS)
    }

    /// Whether the GPU has run without an idle gap for longer than the effort is held after
    /// work stops ([`BusyRun::bound`]).
    pub(super) fn bound(&self) -> bool {
        self.bound
    }

    /// Notes [`BusyRun::bound`]; returns whether that changed.
    pub(super) fn note_bound(&mut self, bound: bool) -> bool {
        let changed = bound != self.bound;
        self.bound = bound;
        changed
    }

    pub(super) fn submit(&mut self) {
        self.streaming = true;
        self.idle_since = None;
        self.submissions = self.submissions.wrapping_add(1);
    }

    /// Count of published submissions, for [`Hold::step`].
    pub(super) fn submissions(&self) -> u64 {
        self.submissions
    }

    pub(super) fn applied(&self) -> u32 {
        self.applied
    }
    pub(super) fn did_apply(&mut self, value: u32) {
        self.applied = value;
        self.confirmed = true;
    }
    pub(super) fn confirmed(&self) -> bool {
        self.confirmed
    }
    pub(super) fn confirm(&mut self) {
        self.confirmed = true;
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
            self.waited = false;
            self.compute_waited = false;
            self.bound = false;
            self.last_wait_ns = 0;
            self.last_compute_wait_ns = 0;
            self.idle_since = None;
            IdleAction::Clear
        } else {
            IdleAction::Recheck(IDLE_HOLD_NS - idle)
        }
    }
}

/// How long each client's commands have kept the GPU running without going idle, from the start
/// and end timestamps the firmware writes for every command it executes.
///
/// The firmware raises its performance state for work that queues up behind the running command,
/// not for how busy the GPU is: a client that submits one command and waits for it before the
/// next keeps the GPU busy with nothing queued, and the state stays at the bottom. Such a loop is
/// told apart from paced work (a frame, then idle until the next one) by its idle gaps. Its GPU
/// only idles for the host's turnaround, shorter than the idle time after which the firmware
/// powers the GPU off ([`FIRMWARE_IDLE_OFF_MS`]), and a gap at least that long ends a run. The
/// host's own hold ([`IDLE_HOLD_MS`]) can be longer than a frame, so it doesn't mark idle. Once
/// a run lasts longer than the effort is held after work stops ([`IDLE_HOLD_NS`]), the GPU is the
/// bottleneck and gets the full effort ([`Effort::note_bound`]). A shorter run would not be worth
/// a floor that outlasts it.
///
/// Runs are kept per client (DRM context). Commands of different clients run side by side and
/// complete in any order: a short command of one client that starts part-way through another
/// client's long render completes first, while the render that keeps the GPU busy through the
/// gap before it has not been noted yet. One run for the whole GPU took that for idle and
/// restarted, the more often the longer the commands, and could lock a GPU-bound client at the
/// floor. A client's own commands follow each other, so its own gaps are real.
///
/// A run counts while its client is still running: until it has been silent for longer than its
/// last command took plus the idle power-off delay, measured on the clock the firmware stamps
/// commands with. A client that stops submitting does not keep the full effort for others.
pub(super) struct BusyRuns {
    runs: [BusyRun; CLIENT_RUNS],
}

/// Clients followed at once; a new client takes the place of the one that ran least recently.
const CLIENT_RUNS: usize = 16;

#[derive(Copy, Clone)]
struct BusyRun {
    /// The entry follows a client.
    used: bool,
    /// The client's execution context id.
    client: u32,
    /// Start of the run, in firmware timestamp ticks.
    start: u64,
    /// The latest end noted.
    last_end: u64,
    /// How long the client's latest command ran.
    last_busy: u64,
}

impl BusyRun {
    const EMPTY: Self = Self {
        used: false,
        client: 0,
        start: 0,
        last_end: 0,
        last_busy: 0,
    };
}

impl BusyRuns {
    pub(super) const fn new() -> Self {
        Self {
            runs: [BusyRun::EMPTY; CLIENT_RUNS],
        }
    }

    /// Notes a command of `client` that executed from `start` to `end`. A start at least
    /// `idle_ticks` (the firmware's idle power-off delay) after every end noted for the client
    /// begins a new run.
    pub(super) fn note(&mut self, client: u32, start: u64, end: u64, idle_ticks: u64) {
        if start == 0 || end <= start {
            return;
        }
        let index = self
            .runs
            .iter()
            .position(|run| run.used && run.client == client)
            .or_else(|| {
                (0..CLIENT_RUNS).min_by_key(|&index| {
                    let run = &self.runs[index];
                    (run.used, run.last_end)
                })
            })
            .unwrap_or(0);
        let run = &mut self.runs[index];
        if !run.used || run.client != client || start >= run.last_end.saturating_add(idle_ticks) {
            *run = BusyRun {
                used: true,
                client,
                start,
                last_end: end,
                last_busy: end - start,
            };
            return;
        }
        run.last_end = run.last_end.max(end);
        run.last_busy = end - start;
    }

    /// Whether some client, still running at `now`, has kept the GPU running for at least
    /// `hold_ticks`.
    pub(super) fn bound(&self, now: u64, hold_ticks: u64, idle_ticks: u64) -> bool {
        self.runs.iter().any(|run| {
            run.used
                && run.last_end.saturating_sub(run.start) >= hold_ticks
                && now.saturating_sub(run.last_end) < run.last_busy.saturating_add(idle_ticks)
        })
    }

    /// The GPU went idle for longer than the effort hold: every client's next command begins a
    /// new run.
    pub(super) fn end(&mut self) {
        *self = Self::new();
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) enum HoldAction {
    None,
    /// Send the keep-powered idle policy.
    Assert,
    /// Send the power-off idle policy again.
    Release,
    Recheck(u64),
}

/// Host-side idle hold. On T6040/25G83 the firmware powers the cores off 2 ms after the last job
/// and reads that delay only at boot; every wake-up then costs about 1 ms. With a longer
/// [`IDLE_HOLD_MS`] the host sends the keep-powered idle policy once work appears and the power-off
/// policy once no work has been submitted or in flight for that long. Checked from the event
/// worker, which runs on every completion, well inside the firmware's 2 ms.
pub(super) struct Hold {
    active: bool,
    seen: u64,
    since: Option<u64>,
}

impl Hold {
    pub(super) const fn new() -> Self {
        Self {
            active: false,
            seen: 0,
            since: None,
        }
    }

    pub(super) fn active(&self) -> bool {
        self.active
    }

    pub(super) fn step(
        &mut self,
        now_ns: u64,
        in_flight: bool,
        submissions: u64,
        hold_ms: u32,
    ) -> HoldAction {
        let enabled = hold_ms > FIRMWARE_IDLE_OFF_MS;
        let fresh = submissions != self.seen;
        self.seen = submissions;
        if in_flight || fresh {
            self.since = None;
        }
        if !self.active {
            return if enabled && (in_flight || fresh) {
                HoldAction::Assert
            } else {
                HoldAction::None
            };
        }
        if in_flight {
            return HoldAction::None;
        }
        let hold_ns = if enabled { u64::from(hold_ms) * 1_000_000 } else { 0 };
        let since = *self.since.get_or_insert(now_ns.max(1));
        let idle = now_ns.saturating_sub(since);
        if idle >= hold_ns {
            HoldAction::Release
        } else {
            HoldAction::Recheck(hold_ns - idle)
        }
    }

    pub(super) fn asserted(&mut self) {
        self.active = true;
    }

    pub(super) fn released(&mut self) {
        self.active = false;
        self.since = None;
    }
}

/// Tracks lockless direct-submission admissions and exits across `idle_check`
/// passes so `Effort` and `Hold` neither leak unarmed after an already-retired
/// direct submission nor clear while a direct publisher is still between
/// admission and publication.
pub(super) struct DirectIdle {
    seen_submissions: u64,
    seen_exits: u64,
}

impl DirectIdle {
    pub(super) const fn new() -> Self {
        Self {
            seen_submissions: 0,
            seen_exits: 0,
        }
    }

    /// Consumes `submissions` and `exits` into `effort`, returning whether
    /// `Effort::idle` and `Hold::step` should treat the GPU as actively busy.
    /// Already-retired direct submissions (`!in_flight && !direct_publishing`)
    /// clear `idle_since` via `effort.submit()` but return `false` so both
    /// state machines still arm their recheck timers.
    pub(super) fn observe(
        &mut self,
        effort: &mut Effort,
        submissions: u64,
        exits: u64,
        in_flight: bool,
        direct_publishing: bool,
    ) -> bool {
        let progressed = submissions != self.seen_submissions || exits != self.seen_exits;
        self.seen_submissions = submissions;
        self.seen_exits = exits;
        if progressed {
            effort.submit();
        }
        in_flight || direct_publishing
    }

    /// Bounded recheck delay while a direct publisher is between admission and
    /// publication/abort, in case it aborts or retires without another wake.
    pub(super) fn recheck_delay(direct_publishing: bool) -> Option<u64> {
        direct_publishing.then_some(IDLE_HOLD_NS)
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

/// Busy-share window: long enough to span frames, short enough to follow a burst.
pub(super) const BUSY_WINDOW_NS: u64 = 200_000_000;
/// A longer gap between reports with no long command in flight was idle: start over
/// rather than average it in.
pub(super) const BUSY_WINDOW_MAX_NS: u64 = 2_000_000_000;
/// An unretired command that has been running continuously for this long saturates
/// its engine and immediately lifts the busy share to 1000 permille, without waiting
/// for it to retire or for the current window to end.
pub(super) const BUSY_LONG_COMMAND_NS: u64 = 50_000_000;

/// Per-window busiest GPU engine share (0..=1000 permille), combining retired stage
/// execution time with unretired in-flight command time.
///
/// The vertex, fragment and compute engines run in parallel, so their summed time is
/// not a share of the window: taking the busiest engine's share avoids double-counting
/// across engines. For each engine, `retired_ns + in_flight_age` is differenced across
/// window boundaries so a command spanning a boundary counts in the window it runs in
/// and is never double-counted when it later retires. Any engine with an unretired
/// command older than [`BUSY_LONG_COMMAND_NS`] lifts immediately to 1000 permille so
/// long compute or render work running beside a light compositor never sits at a low
/// memory floor waiting for retirement. In-flight age counts from publication, so time
/// a command waits in its queue or on dependencies, and a render's vertex and fragment
/// stages retiring together, err toward a higher share, which is the safe direction.
pub(super) struct BusyShare {
    start_ns: Option<u64>,
    retired_ns: [u64; crate::usage::NUM_ENGINES],
    in_flight_ns: [u64; crate::usage::NUM_ENGINES],
    below_top: bool,
}

impl BusyShare {
    pub(super) const fn new() -> Self {
        Self {
            start_ns: None,
            retired_ns: [0; crate::usage::NUM_ENGINES],
            in_flight_ns: [0; crate::usage::NUM_ENGINES],
            below_top: false,
        }
    }

    /// CLPC cleared GPU activity (forgetting its last share so a new burst starts
    /// at the top column). Only called once the GPU is idle: restart the window so
    /// the first report of the next burst re-baselines instead of closing a window
    /// diluted by the idle gap, which would drop the burst off the top column.
    pub(super) fn clear_active(&mut self) {
        self.start_ns = None;
        self.in_flight_ns = [0; crate::usage::NUM_ENGINES];
        self.below_top = false;
    }

    pub(super) fn below_top(&self) -> bool {
        self.below_top
    }

    /// Immediately lifts the reported share to 1000 permille when a sustained busy run
    /// ([`BusyRuns::bound`]) begins while below the top CLPC column, and re-baselines the
    /// measurement window at `now_ns` so short back-to-back commands are neither trapped
    /// at a low memory/fabric floor for up to [`BUSY_WINDOW_NS`] nor diluted by prior
    /// light activity.
    pub(super) fn lift_saturated(
        &mut self,
        now_ns: u64,
        retired_ns: [u64; crate::usage::NUM_ENGINES],
        oldest_in_flight: [u64; crate::usage::NUM_ENGINES],
    ) -> Option<u32> {
        if !self.below_top {
            return None;
        }
        self.start_ns = Some(now_ns);
        self.retired_ns = retired_ns;
        for (slot, &since) in self.in_flight_ns.iter_mut().zip(oldest_in_flight.iter()) {
            *slot = if since != 0 {
                now_ns.saturating_sub(since)
            } else {
                0
            };
        }
        self.below_top = false;
        Some(1000)
    }

    pub(super) fn window_start(&self) -> Option<u64> {
        self.start_ns
    }

    pub(super) fn is_long_in_flight(
        now_ns: u64,
        oldest_in_flight: [u64; crate::usage::NUM_ENGINES],
    ) -> bool {
        oldest_in_flight
            .iter()
            .any(|&since| since != 0 && now_ns.saturating_sub(since) >= BUSY_LONG_COMMAND_NS)
    }

    /// Delay (ns) until an in-flight command reaches [`BUSY_LONG_COMMAND_NS`], at
    /// least 1 ms. Retirement runs its own event pass, so this never delays idle
    /// teardown.
    pub(super) fn recheck_delay_for(now_ns: u64, oldest: u64) -> Option<u64> {
        if oldest == 0 {
            return None;
        }
        let age = now_ns.saturating_sub(oldest);
        Some(BUSY_LONG_COMMAND_NS.saturating_sub(age).max(1_000_000))
    }

    pub(super) fn sample(
        &mut self,
        now_ns: u64,
        retired_ns: [u64; crate::usage::NUM_ENGINES],
        oldest_in_flight: [u64; crate::usage::NUM_ENGINES],
    ) -> Option<u32> {
        let long_in_flight = Self::is_long_in_flight(now_ns, oldest_in_flight);
        let Some(start) = self.start_ns else {
            self.start_ns = Some(now_ns);
            self.retired_ns = retired_ns;
            for (slot, &since) in self.in_flight_ns.iter_mut().zip(oldest_in_flight.iter()) {
                *slot = if since != 0 {
                    now_ns.saturating_sub(since)
                } else {
                    0
                };
            }
            return None;
        };
        let elapsed = now_ns.saturating_sub(start);
        if elapsed < BUSY_WINDOW_NS {
            if self.below_top && long_in_flight {
                self.below_top = false;
                return Some(1000);
            }
            return None;
        }
        let mut busiest = 0u64;
        for i in 0..crate::usage::NUM_ENGINES {
            let now = retired_ns[i];
            let delta = now.saturating_sub(self.retired_ns[i]);
            self.retired_ns[i] = now;
            let since = oldest_in_flight[i];
            let age = if since != 0 {
                now_ns.saturating_sub(since)
            } else {
                0
            };
            let prev_age = self.in_flight_ns[i];
            self.in_flight_ns[i] = age;
            let busy = delta.saturating_add(age).saturating_sub(prev_age);
            busiest = busiest.max(busy);
        }
        self.start_ns = Some(now_ns);
        if elapsed > BUSY_WINDOW_MAX_NS && !long_in_flight {
            return None;
        }
        let permille = if long_in_flight {
            1000
        } else {
            (busiest.saturating_mul(1000) / elapsed).min(1000) as u32
        };
        self.below_top = permille < 1000;
        Some(permille)
    }
}

#[cfg_attr(not(test), kernel::macros::kunit_tests(asahi_g17_idle_hold))]
mod tests {
    use super::*;

    // 24 MHz ticks: a millisecond, the 2 ms idle power-off delay and the 16 ms hold.
    const MS: u64 = 24_000;
    const IDLE: u64 = 2 * MS;
    const HOLD: u64 = 16 * MS;

    /// `count` commands of `client` of `busy` ticks from `start`, each `gap` ticks after the
    /// previous one ended. Returns where the next one would start.
    fn commands(
        runs: &mut BusyRuns,
        client: u32,
        mut start: u64,
        count: u64,
        busy: u64,
        gap: u64,
    ) -> u64 {
        for _ in 0..count {
            runs.note(client, start, start + busy, IDLE);
            start += busy + gap;
        }
        start
    }

    #[test]
    fn a_submit_and_wait_loop_becomes_bound_once_it_outlasts_the_hold() {
        // 1 ms commands, 100 us host turnarounds.
        let mut runs = BusyRuns::new();
        let next = commands(&mut runs, 1, 1000, 10, MS, MS / 10);
        assert!(!runs.bound(next, HOLD, IDLE));
        let next = commands(&mut runs, 1, next, 20, MS, MS / 10);
        assert!(runs.bound(next, HOLD, IDLE));
    }

    #[test]
    fn paced_work_never_becomes_bound() {
        // 2 ms of work every 16.7 ms: each gap is longer than the idle power-off delay.
        let mut runs = BusyRuns::new();
        let next = commands(&mut runs, 1, 1000, 100, 2 * MS, 14 * MS);
        assert!(!runs.bound(next, HOLD, IDLE));
        // A frame whose work leaves less idle than the power-off delay is bound: the GPU is.
        let next = commands(&mut runs, 1, next + 14 * MS, 20, 15 * MS, MS);
        assert!(runs.bound(next, HOLD, IDLE));
    }

    #[test]
    fn another_clients_command_does_not_end_a_run() {
        // Back-to-back 6 ms renders of client 1 keep the GPU busy. 4 ms into each, a 0.2 ms
        // command of client 2 starts and completes first, more than the power-off delay after
        // client 1's previous render ended.
        let mut runs = BusyRuns::new();
        let mut at = 1000;
        for _ in 0..4 {
            runs.note(2, at + 4 * MS, at + 4 * MS + MS / 5, IDLE);
            assert!(!runs.bound(at + 4 * MS + MS / 5, 4 * 6 * MS, IDLE));
            runs.note(1, at, at + 6 * MS, IDLE);
            at += 6 * MS;
        }
        assert!(runs.bound(at, HOLD, IDLE));
        // Still bound while the next render runs and client 2's command completes.
        runs.note(2, at + 4 * MS, at + 4 * MS + MS / 5, IDLE);
        assert!(runs.bound(at + 5 * MS, HOLD, IDLE));
    }

    #[test]
    fn a_client_that_stops_does_not_stay_bound() {
        let mut runs = BusyRuns::new();
        let next = commands(&mut runs, 1, 1000, 30, MS, MS / 10);
        assert!(runs.bound(next, HOLD, IDLE));
        // Silent for its last command plus the power-off delay while another client streams.
        let quiet = next + MS + IDLE;
        commands(&mut runs, 2, next, 4, MS, 4 * MS);
        assert!(!runs.bound(quiet, HOLD, IDLE));
    }

    #[test]
    fn the_least_recent_client_is_replaced_and_end_resets_every_run() {
        let mut runs = BusyRuns::new();
        let next = commands(&mut runs, 1, 1000, 30, MS, MS / 10);
        for client in 2..=CLIENT_RUNS as u32 + 1 {
            runs.note(client, next, next + 1, IDLE);
        }
        // Client 1's entry was the least recent and went to the last new client.
        assert!(!runs.bound(next, HOLD, IDLE));
        let next = commands(&mut runs, 1, next, 30, MS, MS / 10);
        assert!(runs.bound(next, HOLD, IDLE));
        runs.end();
        assert!(!runs.bound(next, 0, IDLE));
        runs.note(1, 0, 10, IDLE);
        runs.note(1, 20, 20, IDLE);
        assert!(!runs.bound(20, 0, IDLE));
    }

    #[test]
    fn effort_bound_is_cleared_with_the_effort() {
        let mut effort = Effort::new();
        effort.submit();
        assert!(effort.note_bound(true));
        assert!(!effort.note_bound(true));
        assert_eq!(effort.idle(1, false), IdleAction::Recheck(IDLE_HOLD_NS));
        assert_eq!(effort.idle(1 + IDLE_HOLD_NS, false), IdleAction::Clear);
        assert!(!effort.bound());
    }

    #[test]
    fn dt_idle_delay_is_bounded_and_absence_preserves_native_policy() {
        assert_eq!(idle_delay(None), Ok(FIRMWARE_IDLE_OFF_MS));
        for delay in [2, 16, 1000] {
            assert_eq!(idle_delay(Some(delay)), Ok(delay));
        }
        for delay in [0, 1, 1001, u32::MAX] {
            assert!(idle_delay(Some(delay)).is_err());
        }
    }

    #[test]
    fn frame_hold_survives_depth_one_gaps_but_releases_at_idle() {
        let mut hold = Hold::new();
        assert_eq!(hold.step(0, true, 1, 16), HoldAction::Assert);
        hold.asserted();
        for submission in 1..=8 {
            // A completed depth-one dispatch followed by another short gap.
            let now = submission * 3_000_000;
            assert_eq!(hold.step(now, false, submission, 16), HoldAction::Recheck(16_000_000));
            assert!(hold.active());
        }
        assert_eq!(hold.step(39_999_999, false, 8, 16), HoldAction::Recheck(1));
        assert_eq!(hold.step(40_000_000, false, 8, 16), HoldAction::Release);
        hold.released();
        assert!(!hold.active());
    }

    #[test]
    fn holds_until_idle_for_the_delay_then_releases() {
        let mut hold = Hold::new();
        assert_eq!(hold.step(0, true, 1, 10), HoldAction::Assert);
        hold.asserted();
        assert_eq!(hold.step(1_000_000, false, 1, 10), HoldAction::Recheck(10_000_000));
        assert_eq!(hold.step(6_000_000, false, 1, 10), HoldAction::Recheck(5_000_000));
        // A new submission restarts the idle period.
        assert_eq!(hold.step(7_000_000, false, 2, 10), HoldAction::Recheck(10_000_000));
        assert_eq!(hold.step(17_000_000, false, 2, 10), HoldAction::Release);
        hold.released();
        assert!(!hold.active());
    }

    #[test]
    fn the_default_delay_never_holds_and_lowering_it_releases() {
        let mut hold = Hold::new();
        assert_eq!(hold.step(0, true, 1, FIRMWARE_IDLE_OFF_MS), HoldAction::None);
        assert_eq!(hold.step(0, true, 2, 10), HoldAction::Assert);
        hold.asserted();
        assert_eq!(hold.step(1, false, 2, FIRMWARE_IDLE_OFF_MS), HoldAction::Release);
    }

    #[test]
    fn direct_idle_arms_recheck_for_retired_submission_and_guards_paused_publisher() {
        const NS_MS: u64 = 1_000_000;
        let mut direct = DirectIdle::new();
        let mut effort = Effort::new();
        let mut hold = Hold::new();

        // 1. Direct submission starts and retires before idle_check() runs:
        //    submissions=1, exits=1, in_flight=false, direct_publishing=false.
        let active = direct.observe(&mut effort, 1, 1, false, false);
        assert!(!active);
        assert!(!effort.will_clear(10 * NS_MS, active));
        assert_eq!(effort.idle(10 * NS_MS, active), IdleAction::Recheck(IDLE_HOLD_NS));
        assert_eq!(DirectIdle::recheck_delay(false), None);
        // On non-M5 Macs with hold_ms > 2, Hold asserts and immediately arms its recheck.
        assert_eq!(hold.step(10 * NS_MS, active, effort.submissions(), 10), HoldAction::Assert);
        hold.asserted();
        assert_eq!(
            hold.step(10 * NS_MS, active, effort.submissions(), 10),
            HoldAction::Recheck(10 * NS_MS)
        );
        // After hold_ms (10 ms) Hold releases; after IDLE_HOLD_NS (16 ms) Effort clears.
        let active = direct.observe(&mut effort, 1, 1, false, false);
        assert_eq!(hold.step(20 * NS_MS, active, effort.submissions(), 10), HoldAction::Release);
        hold.released();
        assert!(!hold.active());
        assert_eq!(effort.idle(20 * NS_MS, active), IdleAction::Recheck(6 * NS_MS));
        assert_eq!(effort.idle(26 * NS_MS, active), IdleAction::Clear);
        assert!(!effort.streaming());

        // 2. Direct publisher is admitted (submissions=2, exits=1) and pauses before
        //    publishing (in_flight=false, direct_publishing=true). Neither Effort nor
        //    Hold may clear/release even far past the 16 ms deadline, and a bounded
        //    recheck remains armed in case the publisher aborts without waking events.
        let active = direct.observe(&mut effort, 2, 1, false, true);
        assert!(active);
        assert_eq!(effort.idle(30 * NS_MS, active), IdleAction::None);
        assert_eq!(DirectIdle::recheck_delay(true), Some(IDLE_HOLD_NS));
        assert_eq!(hold.step(30 * NS_MS, active, effort.submissions(), 10), HoldAction::Assert);
        hold.asserted();
        assert_eq!(hold.step(30 * NS_MS, active, effort.submissions(), 10), HoldAction::None);
        // 100 ms later, still paused: still active, never clears or releases.
        let active = direct.observe(&mut effort, 2, 1, false, true);
        assert!(active);
        assert_eq!(effort.idle(130 * NS_MS, active), IdleAction::None);
        assert_eq!(hold.step(130 * NS_MS, active, effort.submissions(), 10), HoldAction::None);

        // 3. Publisher finishes and retires (exits=2, direct_publishing=false):
        //    exit progression restarts both hold windows cleanly from now_ns.
        let active = direct.observe(&mut effort, 2, 2, false, false);
        assert!(!active);
        assert!(!effort.will_clear(140 * NS_MS, active));
        assert_eq!(effort.idle(140 * NS_MS, active), IdleAction::Recheck(IDLE_HOLD_NS));
        assert_eq!(
            hold.step(140 * NS_MS, active, effort.submissions(), 10),
            HoldAction::Recheck(10 * NS_MS)
        );
        let active = direct.observe(&mut effort, 2, 2, false, false);
        assert_eq!(hold.step(150 * NS_MS, active, effort.submissions(), 10), HoldAction::Release);
        hold.released();
        assert_eq!(effort.idle(156 * NS_MS, active), IdleAction::Clear);
        assert!(!effort.streaming());
    }

    #[test]
    fn long_in_flight_command_lifts_beside_light_submitter() {
        const NS_MS: u64 = 1_000_000;
        let mut share = BusyShare::new();
        // Establish light compositor baseline: ~50 permille (10 ms in 200 ms).
        assert_eq!(share.sample(1000 * NS_MS, [0, 0, 0], [0, 0, 0]), None);
        assert_eq!(
            share.sample(1200 * NS_MS, [2 * NS_MS, 10 * NS_MS, 0], [0, 0, 0]),
            Some(50)
        );
        assert!(share.below_top());
        // A 500 ms compute job starts at t=1250 ms; compositor submits at t=1410 ms.
        assert_eq!(
            share.sample(
                1410 * NS_MS,
                [4 * NS_MS, 20 * NS_MS, 0],
                [0, 0, 1250 * NS_MS],
            ),
            Some(1000)
        );
        assert!(!share.below_top());
        // Still running across the next window at t=1610 ms: stays at 1000.
        assert_eq!(
            share.sample(
                1610 * NS_MS,
                [6 * NS_MS, 30 * NS_MS, 0],
                [0, 0, 1250 * NS_MS],
            ),
            Some(1000)
        );
        // Retires at t=1750 ms (+500 ms retired); only the 140 ms in [1610, 1750] counts.
        assert_eq!(
            share.sample(1810 * NS_MS, [8 * NS_MS, 40 * NS_MS, 500 * NS_MS], [0, 0, 0]),
            Some(700)
        );
        // Next window returns cleanly to the light compositor share.
        assert_eq!(
            share.sample(2010 * NS_MS, [10 * NS_MS, 50 * NS_MS, 500 * NS_MS], [0, 0, 0]),
            Some(50)
        );
    }

    #[test]
    fn late_long_command_lifts_mid_window_without_double_counting() {
        const NS_MS: u64 = 1_000_000;
        let mut share = BusyShare::new();
        assert_eq!(share.sample(1000 * NS_MS, [0, 0, 0], [0, 0, 0]), None);
        assert_eq!(share.sample(1200 * NS_MS, [0, 10 * NS_MS, 0], [0, 0, 0]), Some(50));
        // Long compute starts at t=1390 ms; window closes at t=1410 ms (only 20 ms old).
        assert_eq!(
            share.sample(1410 * NS_MS, [0, 20 * NS_MS, 0], [0, 0, 1390 * NS_MS]),
            Some(95)
        );
        assert!(share.below_top());
        // At t=1440 ms (50 ms old, mid-window), early lift reports 1000 immediately.
        assert_eq!(
            share.sample(1440 * NS_MS, [0, 22 * NS_MS, 0], [0, 0, 1390 * NS_MS]),
            Some(1000)
        );
        assert!(!share.below_top());
        // Second mid-window submit does not spam CLPC.
        assert_eq!(
            share.sample(1460 * NS_MS, [0, 23 * NS_MS, 0], [0, 0, 1390 * NS_MS]),
            None
        );
        // Retires at t=1490 ms (+100 ms retired); window [1410, 1610] credits exact 80 ms.
        assert_eq!(
            share.sample(1610 * NS_MS, [0, 30 * NS_MS, 100 * NS_MS], [0, 0, 0]),
            Some(400)
        );
    }

    #[test]
    fn straddling_short_command_and_long_gaps_behave_cleanly() {
        const NS_MS: u64 = 1_000_000;
        let mut share = BusyShare::new();
        assert_eq!(share.sample(1000 * NS_MS, [0, 0, 0], [0, 0, 0]), None);
        // 40 ms command [1170, 1210] straddles boundary 1200: 30 ms in first, 10 ms in second.
        assert_eq!(
            share.sample(1200 * NS_MS, [0, 0, 0], [0, 0, 1170 * NS_MS]),
            Some(150)
        );
        assert_eq!(
            share.sample(1400 * NS_MS, [0, 0, 40 * NS_MS], [0, 0, 0]),
            Some(50)
        );
        // Multi-engine takes busiest engine rather than summing parallel engines.
        assert_eq!(
            share.sample(1600 * NS_MS, [0, 120 * NS_MS, 150 * NS_MS], [0, 0, 0]),
            Some(600)
        );
        // >2 s idle gap with a freshly-started command (age 0) restarts without reporting.
        assert_eq!(
            share.sample(5000 * NS_MS, [0, 120 * NS_MS, 150 * NS_MS], [0, 0, 5000 * NS_MS]),
            None
        );
        // >2 s active command still reports 1000.
        assert_eq!(
            share.sample(8000 * NS_MS, [0, 120 * NS_MS, 150 * NS_MS], [0, 0, 5000 * NS_MS]),
            Some(1000)
        );
        // Recheck delay wakes once at the long-command age (at least 1 ms), None when idle.
        assert_eq!(BusyShare::recheck_delay_for(1010 * NS_MS, 0), None);
        assert_eq!(
            BusyShare::recheck_delay_for(1010 * NS_MS, 1000 * NS_MS),
            Some(40 * NS_MS)
        );
        assert_eq!(
            BusyShare::recheck_delay_for(1045 * NS_MS, 1000 * NS_MS),
            Some(5 * NS_MS)
        );
        assert_eq!(
            BusyShare::recheck_delay_for(1100 * NS_MS, 1000 * NS_MS),
            Some(NS_MS)
        );
    }

    #[test]
    fn new_burst_after_idle_restarts_the_window() {
        const NS_MS: u64 = 1_000_000;
        let mut share = BusyShare::new();
        // Steady 600 permille fragment work.
        assert_eq!(share.sample(900 * NS_MS, [0, 0, 0], [0, 0, 0]), None);
        assert_eq!(share.sample(1100 * NS_MS, [0, 120 * NS_MS, 0], [0, 0, 0]), Some(600));
        assert_eq!(share.sample(1300 * NS_MS, [0, 240 * NS_MS, 0], [0, 0, 0]), Some(600));
        // Work continues to 1400 ms, then the GPU idles and CLPC clears its share.
        share.clear_active();
        assert!(!share.below_top());
        // First submission after a 600 ms idle gap: closing [1300, 2000] would report
        // a diluted 85 permille (60 ms in 700 ms); it re-baselines instead.
        assert_eq!(share.sample(2000 * NS_MS, [0, 300 * NS_MS, 0], [0, 0, 0]), None);
        assert!(!share.below_top());
        // No report until a fresh 200 ms window of the new burst completes.
        assert_eq!(share.sample(2190 * NS_MS, [0, 414 * NS_MS, 0], [0, 0, 0]), None);
        assert_eq!(share.sample(2200 * NS_MS, [0, 420 * NS_MS, 0], [0, 0, 0]), Some(600));
    }

    #[test]
    fn sustained_busy_run_lifts_short_dispatches_mid_window() {
        const NS_MS: u64 = 1_000_000;
        let mut share = BusyShare::new();
        // Light compositor establishes a 50 permille floor (below top).
        assert_eq!(share.sample(1000 * NS_MS, [0, 0, 0], [0, 0, 0]), None);
        assert_eq!(share.sample(1200 * NS_MS, [0, 10 * NS_MS, 0], [0, 0, 0]), Some(50));
        assert!(share.below_top());
        // Short (<50 ms) compute dispatches saturate the GPU starting at t=1300 ms.
        // At t=1316 ms BusyRuns::bound triggers lift_saturated(), lifting immediately.
        assert_eq!(
            share.lift_saturated(1316 * NS_MS, [0, 14 * NS_MS, 16 * NS_MS], [0, 0, 0]),
            Some(1000)
        );
        assert!(!share.below_top());
        // Repeated calls while already at top are a no-op.
        assert_eq!(
            share.lift_saturated(1332 * NS_MS, [0, 14 * NS_MS, 32 * NS_MS], [0, 0, 0]),
            None
        );
        // The re-baselined 200 ms window [1316, 1516] measures the burst cleanly (1000 permille).
        assert_eq!(
            share.sample(1516 * NS_MS, [0, 14 * NS_MS, 216 * NS_MS], [0, 0, 0]),
            Some(1000)
        );
        assert!(!share.below_top());
        // Once the burst ends, the next window returns cleanly to the light compositor share.
        assert_eq!(
            share.sample(1716 * NS_MS, [0, 24 * NS_MS, 216 * NS_MS], [0, 0, 0]),
            Some(50)
        );
        assert!(share.below_top());
    }

    #[test]
    fn compute_waiters_lift_immediately_and_expire_while_streaming() {
        const NS_MS: u64 = 1_000_000;
        let mut effort = Effort::new();
        effort.submit();
        assert!(!effort.compute_waited());
        // Render waiter alone never marks compute_waited.
        assert!(effort.note_waiters(10 * NS_MS, (1, 1)));
        assert!(!effort.note_compute_waiters(10 * NS_MS, (0, 0)));
        assert!(!effort.compute_waited());
        // A pure-compute waiter lifts immediately.
        assert!(effort.note_compute_waiters(11 * NS_MS, (1, 1)));
        assert!(effort.compute_waited());
        // Holds across a short host turnaround between depth-1 dispatches.
        assert!(!effort.note_compute_waiters(15 * NS_MS, (0, 1)));
        assert!(effort.compute_waited());
        // Expires cleanly after IDLE_HOLD_NS even while a compositor keeps streaming.
        assert!(effort.note_compute_waiters(11 * NS_MS + IDLE_HOLD_NS, (0, 1)));
        assert!(!effort.compute_waited());
        // Idle clear also resets compute_waited.
        assert!(effort.note_compute_waiters(40 * NS_MS, (1, 2)));
        assert!(effort.compute_waited());
        assert_eq!(effort.idle(40 * NS_MS, false), IdleAction::Recheck(IDLE_HOLD_NS));
        assert!(effort.will_clear(40 * NS_MS + IDLE_HOLD_NS, false));
        assert_eq!(effort.idle(40 * NS_MS + IDLE_HOLD_NS, false), IdleAction::Clear);
        assert!(!effort.compute_waited());
    }
}

unsafe extern "C" {
    fn asahi_clpc_gpu_get(dev: *mut core::ffi::c_void, out: *mut *mut core::ffi::c_void) -> core::ffi::c_int;
    fn asahi_clpc_gpu_set_active(gpu: *mut core::ffi::c_void, active: bool);
    fn asahi_clpc_gpu_put(gpu: *mut core::ffi::c_void);
    fn asahi_clpc_gpu_set_busy(gpu: *mut core::ffi::c_void, permille: core::ffi::c_uint);
}

/// DT-selected memory-demand provider. Its independent allocation outlives a
/// detached provider; a device link orders supplier removal and suspend.
pub(super) struct Demand(*mut core::ffi::c_void);

// SAFETY: the provider's leaf spinlock serializes all access; no GPU callback.
unsafe impl Send for Demand {}
// SAFETY: shared access only changes that locked request, never its lifetime.
unsafe impl Sync for Demand {}

impl Demand {
    pub(super) fn new(dev: *mut kernel::bindings::device) -> kernel::error::Result<Self> {
        let mut out = core::ptr::null_mut();
        // SAFETY: live platform device and writable result; C returns an owned reference.
        let ret = unsafe { asahi_clpc_gpu_get(dev.cast(), &mut out) };
        if ret < 0 {
            Err(kernel::error::Error::from_errno(ret))
        } else {
            Ok(Self(out))
        }
    }

    pub(super) fn has_provider(&self) -> bool {
        !self.0.is_null()
    }

    pub(super) fn set_active(&self, active: bool) {
        // SAFETY: the retained reference is live; NULL denotes no DT provider.
        unsafe { asahi_clpc_gpu_set_active(self.0, active) };
    }

    pub(super) fn set_busy(&self, permille: u32) {
        // SAFETY: the retained reference is live; NULL denotes no DT provider.
        unsafe { asahi_clpc_gpu_set_busy(self.0, permille.min(1000) as core::ffi::c_uint) };
    }
}

impl Drop for Demand {
    fn drop(&mut self) {
        // SAFETY: releases exactly the reference returned by get, clearing demand first.
        unsafe { asahi_clpc_gpu_put(self.0) };
    }
}
