// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded isolation for a compute watchdog blocked behind another VM's older kick.
//! Publication order is evidence of a possible blocker, not proof of a firmware
//! dependency. Renewals are bounded; the oldest unfinished queue is never excused.
//! A blocker that runs inside the watchdog allowance its queue asked for excuses
//! the work behind it for that long ([`decide`]), bounded by the longest allowance
//! the system permits ([`excuse_limit`]).

pub(crate) const MAX_RENEWALS: u8 = 8;

/// A borrowed render graph or queued off-lock retirement is not evidence of
/// progress. Share the finite deferral bound with cross-VM attribution.
pub(crate) fn transaction_grace(renewals: u8) -> bool { renewals < MAX_RENEWALS }

/// Keep the scheduler poll, control-ring waits and ordinary jobs at two seconds.
/// Native ray traversal and AS builds can exceed one poll (CTS stress). Four
/// extra polls give it ten seconds, still one tenth of the legacy scheduler's
/// fallback. This is an execution allowance, not evidence of GPU progress, and
/// is consumed once per scheduler job, independently of cross-VM/replay grace.
pub(crate) const POLL_MS: u32 = 2000;
const EXECUTION_RENEWALS: u8 = 4;

/// How long an innocent client's publication waits for a recovery to settle and its
/// spared work to drain, instead of failing with EBUSY: the watchdog defers work behind
/// another VM's failed work at most this long before it settles it (`classify`; ray work
/// gets the execution renewals on top). A failed blocker excuses nothing ([`decide`]).
pub(crate) const RECOVERY_WAIT_MS: u32 =
    POLL_MS * (1 + EXECUTION_RENEWALS as u32 + MAX_RENEWALS as u32);

/// A background control/replay poll has no scheduler fence to expire it.
/// Its timestamp belongs to the exact transaction and is never renewed by work.
pub(crate) fn background_stalled(since: Option<u64>, now: u64) -> bool {
    since.is_some_and(|since| now.saturating_sub(since) >= 10_000_000_000)
}

/// Input comes only from fully validated commands. Render metadata covers the
/// VDM half (vertex/tessellation/geometry) and the fragment half independently.
/// AS builders do not traverse rays and need an explicit, bounded UAPI hint.
pub(crate) fn select(compute_ray: bool, render_ray: [bool; 2], as_build: bool) -> bool {
    compute_ray || render_ray.into_iter().any(|enabled| enabled) || as_build
}

/// Firmware dependencies can hold an untagged helper behind a long command on
/// the other engine. Classify the complete validated ioctl before enqueueing
/// anything; no state or allowance leaks into a later submission.
pub(crate) fn select_submission(commands: impl Iterator<Item = bool>) -> bool {
    commands.into_iter().any(|enabled| enabled)
}

/// Only the explicit AS-build bit is newly accepted; reserved and unrelated
/// flags remain invalid on G17P. Return None before any command is published.
pub(crate) fn as_build(flags: u32) -> Option<bool> {
    const AS_BUILD: u32 = 1 << 2;
    if flags & !AS_BUILD != 0 { None } else { Some(flags & AS_BUILD != 0) }
}

/// The watchdog allowance of a compute command unless its queue asked for
/// another (`drm_asahi_queue_create::compute_timeout_ms`): one watchdog poll.
pub(crate) const DEFAULT_ALLOWANCE_MS: u32 = POLL_MS;

/// The allowance a queue gets for its compute commands when it asks for
/// `requested_ms`, given the longest the system allows (`maximum_ms`, the
/// `compute_timeout_max_ms` module parameter): never below the default.
pub(crate) fn allowance_ms(requested_ms: u32, maximum_ms: u32) -> u32 {
    requested_ms.min(maximum_ms).max(DEFAULT_ALLOWANCE_MS)
}

/// A job's watchdog allowance: how long the watchdog renews it for the asking,
/// counted from about when it reached the head of its queue.
///
/// It is not a deadline. The watchdog only sees a job at its expiries, and
/// does not know when the job started: the scheduler arms a queue's watchdog
/// when a job reaches the head (the job ahead of it completed) and again
/// whenever a later job of the queue is pushed, and the watchdog expires one
/// [`POLL_MS`] later. So a job's first expiry comes at least one poll after it
/// reached the head, later if pushes postponed it, and the time is counted
/// from one poll before that expiry. A job still in publication at an expiry
/// (waiting for capacity) has not started: its time counts from the first
/// expiry that finds it published. Either way a job never loses time to the
/// jobs ahead of it, and it may get up to one poll more than its allowance.
/// Under the default allowance it is never renewed. A job past its allowance
/// is not failed for that alone: the caller's other renewals and deferrals
/// are tried next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Allowance {
    ms: u32,
    /// When the job reached the head, once an expiry placed it.
    head_since: Option<u64>,
    /// An expiry found the job still in publication.
    waited: bool,
}

impl Allowance {
    pub(crate) const fn new(ms: u32) -> Self {
        Self { ms, head_since: None, waited: false }
    }

    /// Whether the job is within its allowance at the expiry at `now` (host ns).
    pub(crate) fn renew(&mut self, published: bool, now: u64) -> bool {
        if !published {
            self.waited = true;
            return false;
        }
        let poll = if self.waited { 0 } else { u64::from(POLL_MS) * 1_000_000 };
        let since = *self.head_since.get_or_insert(now.saturating_sub(poll));
        now.saturating_sub(since) < u64::from(self.ms) * 1_000_000
    }

    /// Whether the queue asked for an allowance longer than the default.
    pub(crate) fn asked(&self) -> bool {
        self.ms > DEFAULT_ALLOWANCE_MS
    }

    /// Whether a [`Self::renew`] at an expiry that finds the job `published` places it after a
    /// wait in publication: the one poll even the default allowance renews.
    pub(crate) fn placing_after_wait(&self, published: bool) -> bool {
        published && self.waited && self.head_since.is_none()
    }

    /// When the allowance ends (host ns), once an expiry placed the job, for an allowance
    /// longer than the default: until then the watchdog renews the job for the asking. A
    /// default allowance renews at most the poll after a wait in publication and excuses
    /// nobody ([`Head::excused`]).
    pub(crate) fn end(&self) -> Option<u64> {
        let since = self.head_since.filter(|_| self.asked())?;
        Some(since.saturating_add(u64::from(self.ms) * 1_000_000))
    }
}

/// How many polls a job may be excused behind other VMs' live work inside those blockers'
/// allowances ([`decide`]): the longest allowance a queue may get (`maximum_ms`, the
/// `compute_timeout_max_ms` module parameter) in whole polls, 15 by default.
pub(crate) fn excuse_limit(maximum_ms: u32) -> u32 {
    allowance_ms(u32::MAX, maximum_ms).div_ceil(POLL_MS)
}

/// The polls a waiting job was excused so far (`used`) and the most it may be (`limit`,
/// [`excuse_limit`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Excuse {
    pub(crate) used: u32,
    pub(crate) limit: u32,
}

pub(crate) fn renew_execution(published: bool, enabled: bool, renewals: &mut u8) -> bool {
    if !published || !enabled || *renewals >= EXECUTION_RENEWALS {
        return false;
    }
    *renewals += 1;
    true
}

/// Polls a published job may be renewed while the GPU keeps retiring work. A job
/// shares the GPU's cores with every other runnable queue, so its wall time grows
/// with the other clients' work: beside four other clients a 1.5 s dispatch took up
/// to 5 s, and lower-priority work waits behind higher-priority work. It has not
/// hung while the GPU keeps completing commands. Fourteen renewals bound the wait at
/// thirty seconds, after which a job that still has not finished fails as before.
const SHARED_RENEWALS: u8 = 14;

/// The GPU counts as live while some command retired this recently. Under heavy
/// sharing the longest gap between retirements measured 1.2 s; a GPU that retires
/// nothing for this long has stopped.
pub(crate) const LIVENESS_NS: u64 = 5_000_000_000;

/// Whether some command retired within [`LIVENESS_NS`] (`since_retired`).
pub(crate) fn retiring(since_retired: Option<u64>) -> bool {
    since_retired.is_some_and(|since| since < LIVENESS_NS)
}

/// Renews a published job's deadline while the GPU is `shared`, at most
/// [`SHARED_RENEWALS`] times. It is shared while some command retires
/// ([`retiring`]), or while a long job of another queue runs beside a long job:
/// two ray traversal dispatches of about 6 s each took 10 to 17 s side by side, and
/// neither retires anything until it ends.
pub(crate) fn renew_shared(published: bool, shared: bool, renewals: &mut u8) -> bool {
    if !published || !shared || *renewals >= SHARED_RENEWALS {
        return false;
    }
    *renewals += 1;
    true
}

/// Counts a poll that an allowance renewed beyond what the default allowance renews
/// (`beyond_default`) as one of the job's shared renewals. The two run from the same start
/// instead of one after the other: where [`renew_shared`] applies, a job is renewed for the
/// longer of its allowance and the thirty seconds sharing gives any job, not for their sum (58 s
/// with the default maximum). The one poll every allowance renews after a wait in publication
/// ([`Allowance::placing_after_wait`]) is not counted, so that asking never leaves a job fewer
/// polls than the default.
pub(crate) fn count_allowance_poll(beyond_default: bool, renewals: &mut u8) {
    if beyond_default {
        *renewals = renewals.saturating_add(1).min(SHARED_RENEWALS);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Head {
    /// The owning VM's [`crate::mmu::Vm::id`], one per VM status; the watchdog lines print it.
    pub(crate) vm: u64,
    pub(crate) published: u64,
    pub(crate) finished: bool,
    /// Its queue already failed (quarantined) and holds the GPU only until the firmware drains
    /// it. No later watchdog of its own will settle it.
    pub(crate) failed: bool,
    /// Its own watchdog still renews it for the allowance its queue asked for
    /// ([`Allowance::end`]): a long command running as allowed, not a hung one.
    pub(crate) excused: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Decision {
    Own,
    /// Renewed: counts against [`MAX_RENEWALS`].
    Defer,
    /// Renewed behind a live blocker inside its allowance: counts against [`Excuse::limit`]
    /// only.
    Excuse,
    Spare,
}

/// Another VM's older unfinished work that can block `own`: live work before work whose queue
/// already failed, the oldest of each first.
pub(crate) fn blocker(own: Head, others: impl Iterator<Item = Head>) -> Option<Head> {
    others
        .filter(|other| other.vm != own.vm && !other.finished && other.published < own.published)
        .min_by_key(|other| (other.failed, other.published))
}

/// Work behind another VM's older unfinished work is renewed up to MAX_RENEWALS times, so that
/// the blocker's own failure and teardown can free the GPU. After that it is spared only behind
/// a live blocker, which its own watchdog still settles. A blocker that already failed waits only
/// for the firmware to drain it and no longer excuses later work: otherwise an innocent frame
/// published before the culprit, which timed out first, would spare the culprit, and a pair that
/// never drains would spare every later timeout.
///
/// A live blocker that its own watchdog still renews for its queue's allowance
/// ([`Head::excused`]) is not hung: its command may run that long, and work queued behind it
/// on the GPU waits for it. Such work is renewed without using any more of its MAX_RENEWALS,
/// so that what is left of that grace covers the blocker's own failure and teardown once the
/// allowance has passed; then the rules above apply again.
///
/// The excuse is bounded by the waiting job's [`Excuse::limit`], the longest allowance the
/// system permits in polls, however many blockers take turns. This attribution therefore
/// renews a job at most `limit + MAX_RENEWALS` times: 23 with the default 30 s maximum, 46 s
/// of renewals after the expiry that first deferred it. That bounds this rule only. A job's
/// own allowance and its other deferrals ([`renew_execution`], [`renew_shared`]) are counted
/// separately and come on top, and each renewal re-arms a timer rather than an absolute
/// deadline, so the time until the job is failed (spared) can exceed 46 s.
pub(crate) fn decide(own: Head, renewals: u8, excuse: Excuse, blocker: Option<Head>) -> Decision {
    match blocker {
        _ if own.finished => Decision::Own,
        None => Decision::Own,
        Some(blocker) if blocker.excused && !blocker.failed && excuse.used < excuse.limit => {
            Decision::Excuse
        }
        Some(_) if renewals < MAX_RENEWALS => Decision::Defer,
        Some(blocker) if blocker.failed => Decision::Own,
        Some(_) => Decision::Spare,
    }
}

/// A watchdog's decision with the heads it rests on, for its log line.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Verdict {
    pub(crate) decision: Decision,
    pub(crate) own: Head,
    pub(crate) blocker: Option<Head>,
}

impl Verdict {
    /// The blocker a watchdog line names: at the first excuse, at the first counted deferral
    /// and once the grace has run out.
    pub(crate) fn reported_blocker(&self, renewals: u8, excuse: Excuse) -> Option<Head> {
        self.blocker.filter(|_| match self.decision {
            Decision::Excuse => excuse.used == 0,
            Decision::Defer => renewals == 0,
            Decision::Own | Decision::Spare => true,
        })
    }
}

pub(crate) fn attribute(own: Head, renewals: u8, excuse: Excuse,
    others: impl Iterator<Item = Head>) -> Verdict {
    let blocker = blocker(own, others);
    Verdict {
        decision: decide(own, renewals, excuse, blocker),
        own,
        blocker,
    }
}

/// The progress watchdog's interval: the firmware counts as progressing for this long after a
/// read found its progress word changed.
const PROGRESS_INTERVAL_NS: u64 = 5_000_000_000;

/// The firmware's progress word as a watchdog last read it, and [`crate::g17::now_ns`] of the
/// latest read that found it changed (zero before the first).
static PROGRESS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
static LAST_PROGRESS_NS: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);

/// A watchdog read the firmware's progress word. Returns whether the firmware changed it within
/// [`PROGRESS_INTERVAL_NS`].
pub(crate) fn note_progress(word: u32) -> bool {
    use core::sync::atomic::Ordering;
    let now = crate::g17::now_ns();
    if PROGRESS.swap(word, Ordering::Relaxed) != word {
        LAST_PROGRESS_NS.store(now, Ordering::Relaxed);
    }
    match LAST_PROGRESS_NS.load(Ordering::Relaxed) {
        0 => false,
        last => now.saturating_sub(last) < PROGRESS_INTERVAL_NS,
    }
}

/// Work whose context has an older node in use ahead of its own is not late: the firmware
/// lists a context's nodes by their words and flushes from the head, so this work's stamps wait
/// behind the older node until it drains. A published job behind its context's older node is
/// renewed while the firmware's progress word moves, with two limits:
///
/// - Not once its VM has failed work (`failed`). The older node's holder may be that failed
///   work, held until the firmware drains it, and a failed blocker excuses nothing ([`decide`]).
/// - At most [`SHARED_RENEWALS`] times, counted with the job's other deferrals (`renewals`).
///   The older node's own job runs under the same allowances and started earlier, so a healthy
///   older node drains or fails within that budget; past it the job times out as any other.
///
/// A firmware that stops progressing for the interval expires the job as before, and an older
/// node of another VM still goes through [`attribute`].
pub(crate) fn behind_own_head(published: bool, older: Option<u32>, progressing: bool,
    failed: bool, renewals: u8) -> bool {
    published && older.is_some() && progressing && !failed && renewals < SHARED_RENEWALS
}

#[cfg_attr(not(test), kernel::macros::kunit_tests(asahi_g17_execution_allowance))]
mod tests {
    use super::*;
    const MS: u64 = 1_000_000;
    #[test]
    fn a_queue_gets_the_allowance_it_asks_for_within_bounds() {
        assert_eq!(allowance_ms(0, 30_000), DEFAULT_ALLOWANCE_MS);
        assert_eq!(allowance_ms(500, 30_000), DEFAULT_ALLOWANCE_MS);
        assert_eq!(allowance_ms(20_000, 30_000), 20_000);
        assert_eq!(allowance_ms(u32::MAX, 30_000), 30_000);
        // A maximum below the default still leaves the default.
        assert_eq!(allowance_ms(20_000, 0), DEFAULT_ALLOWANCE_MS);
    }
    #[test]
    fn the_default_allowance_renews_nothing() {
        let mut allowance = Allowance::new(DEFAULT_ALLOWANCE_MS);
        assert!(!allowance.renew(true, 100_000 * MS));
        // Not after a wait in publication either.
        let mut allowance = Allowance::new(DEFAULT_ALLOWANCE_MS);
        assert!(!allowance.renew(false, 100_000 * MS));
        assert!(allowance.renew(true, 102_000 * MS));
        assert!(!allowance.renew(true, 104_000 * MS));
    }
    #[test]
    fn a_long_allowance_runs_from_the_head_of_the_queue() {
        // Reached the head at 10 s (the job ahead completed): first expiry at 12 s.
        let start = 10_000 * MS;
        let poll = u64::from(POLL_MS) * MS;
        let mut allowance = Allowance::new(30_000);
        let mut expiry = start + poll;
        while allowance.renew(true, expiry) {
            expiry += poll;
        }
        assert_eq!(allowance.head_since, Some(start));
        // Killed at the first poll at or past 30 s after it reached the head.
        assert_eq!(expiry - start, 30_000 * MS);
        // A first expiry delayed by later pushes only moves the count later.
        let mut allowance = Allowance::new(30_000);
        assert!(allowance.renew(true, start + poll + 500 * MS));
        assert_eq!(allowance.head_since, Some(start + 500 * MS));
    }
    #[test]
    fn a_job_waiting_in_publication_starts_at_its_first_published_expiry() {
        let mut allowance = Allowance::new(30_000);
        assert!(!allowance.renew(false, 10_000 * MS));
        assert_eq!(allowance.head_since, None);
        assert!(allowance.renew(true, 12_000 * MS));
        assert_eq!(allowance.head_since, Some(12_000 * MS));
        assert!(allowance.renew(true, 41_999 * MS));
        assert!(!allowance.renew(true, 42_000 * MS));
    }
    #[test]
    fn selects_compute_both_render_halves_and_builds() {
        assert!(!select(false, [false; 2], false));
        assert!(select(true, [false; 2], false));
        assert!(select(false, [true, false], false));
        assert!(select(false, [false, true], false));
        assert!(select(false, [true; 2], false));
        assert!(select(false, [false; 2], true));
    }
    #[test]
    fn helpers_share_only_the_current_submission_allowance() {
        assert!(select_submission([false, true, false].into_iter()));
        assert!(select_submission([true, false].into_iter()));
        assert!(select_submission([false, true].into_iter()));
        assert!(!select_submission([false, false].into_iter()));
        assert!(!select_submission([].into_iter()));
        let mut helper = 0;
        for _ in 0..4 { assert!(renew_execution(true, true, &mut helper)); }
        assert!(!renew_execution(true, true, &mut helper));
        assert!(!renew_execution(true, false, &mut 0));
    }
    #[test]
    fn build_hint_rejects_reserved_and_unknown_flags() {
        assert_eq!(as_build(0), Some(false));
        assert_eq!(as_build(1 << 2), Some(true));
        for bit in 0..32 {
            if bit == 2 { continue; }
            assert_eq!(as_build(1 << bit), None);
            assert_eq!(as_build((1 << bit) | (1 << 2)), None);
        }
        assert_eq!(as_build(u32::MAX), None);
    }
    #[test]
    fn borrowed_render_and_queued_retirement_cannot_defer_forever() {
        for count in 0..=u8::MAX {
            assert_eq!(transaction_grace(count), count < MAX_RENEWALS);
        }
    }
    #[test]
    fn background_control_and_replay_polls_have_an_absolute_deadline() {
        let since = 42;
        assert!(!background_stalled(None, u64::MAX));
        assert!(!background_stalled(Some(since), since - 1));
        assert!(!background_stalled(Some(since), since + 9_999_999_999));
        for _ in 0..1024 {
            assert!(background_stalled(Some(since), since + 10_000_000_000));
        }
        assert!(background_stalled(Some(since), u64::MAX));
    }
    /// No excuse used yet, under the default maximum allowance.
    const FRESH: Excuse = Excuse { used: 0, limit: 15 };
    fn classify(own: Head, renewals: u8, others: impl Iterator<Item = Head>) -> Decision {
        attribute(own, renewals, FRESH, others).decision
    }
    fn head(vm: u64, published: u64, finished: bool) -> Head {
        Head { vm, published, finished, failed: false, excused: false }
    }
    fn failed(vm: u64, published: u64) -> Head {
        Head { vm, published, finished: false, failed: true, excused: false }
    }
    /// A live head whose own watchdog still renews it for its allowance.
    fn excused(vm: u64, published: u64) -> Head {
        Head { excused: true, ..head(vm, published, false) }
    }
    #[test]
    fn ray_query_execution_is_bounded_at_ten_seconds() {
        let mut renewals = 0;
        for poll in 1..=4 {
            assert!(renew_execution(true, true, &mut renewals));
            assert_eq!(renewals, poll);
        }
        for _ in 0..256 {
            assert!(!renew_execution(true, true, &mut renewals));
        }
        assert_eq!(POLL_MS * (u32::from(renewals) + 1), 10_000);
    }
    #[test]
    fn ordinary_and_unpublished_jobs_keep_the_first_deadline() {
        for (published, enabled) in [(false, false), (false, true), (true, false)] {
            let mut renewals = 0;
            assert!(!renew_execution(published, enabled, &mut renewals));
            assert_eq!(renewals, 0);
        }
    }
    #[test]
    fn ray_query_and_cross_vm_credits_are_independent_and_finite() {
        let mut ray = 0;
        let own = head(5, 101, false);
        for _ in 0..4 {
            assert!(renew_execution(true, true, &mut ray));
        }
        for victim in 0..MAX_RENEWALS {
            assert!(!renew_execution(true, true, &mut ray));
            assert_eq!(classify(own, victim, [head(7, 100, false)].into_iter()), Decision::Defer);
        }
        assert_eq!(classify(own, MAX_RENEWALS, [head(7, 100, false)].into_iter()), Decision::Spare);
        assert_eq!(POLL_MS * (1 + u32::from(ray) + u32::from(MAX_RENEWALS)), 26_000);
    }
    #[test]
    fn a_live_gpu_renews_a_published_job_for_thirty_seconds() {
        let mut renewals = 0;
        for poll in 1..=14u8 {
            assert!(renew_shared(true, retiring(Some(4_900_000_000)), &mut renewals));
            assert_eq!(renewals, poll);
        }
        assert!(!renew_shared(true, true, &mut renewals));
        assert_eq!(POLL_MS * (u32::from(renewals) + 1), 30_000);
    }
    /// When an ordinary job that never finishes is failed (ms), on a GPU that is `shared`
    /// throughout or not at all, by the allowance and the shared renewals in the order the
    /// watchdog applies them, with no execution renewal or deferral. With `waited`, the expiry
    /// at 2 s finds it still in publication (a deferral keeps it) and the next one published.
    fn fails_at(allowance_ms: u32, shared: bool, waited: bool) -> u32 {
        let mut allowance = Allowance::new(allowance_ms);
        let mut renewals = 0;
        let mut expiry = POLL_MS;
        if waited {
            assert!(!allowance.renew(false, u64::from(expiry) * MS));
            expiry += POLL_MS;
        }
        loop {
            let placing = allowance.placing_after_wait(true);
            if allowance.renew(true, u64::from(expiry) * MS) {
                count_allowance_poll(allowance.asked() && !placing, &mut renewals);
            } else if !renew_shared(true, shared, &mut renewals) {
                return expiry;
            }
            expiry += POLL_MS;
            assert!(expiry < 1_000_000);
        }
    }
    #[test]
    fn an_allowance_and_the_shared_renewals_overlap() {
        // Under the default allowance: one poll alone, thirty seconds on a shared GPU.
        assert_eq!(fails_at(DEFAULT_ALLOWANCE_MS, false, false), 2_000);
        assert_eq!(fails_at(DEFAULT_ALLOWANCE_MS, true, false), 30_000);
        // The longest allowance is the limit with or without sharing: not 30 s + 28 s.
        assert_eq!(fails_at(30_000, false, false), 30_000);
        assert_eq!(fails_at(30_000, true, false), 30_000);
        // A shorter one never leaves a job worse off than the default on a shared GPU.
        assert_eq!(fails_at(6_000, false, false), 6_000);
        assert_eq!(fails_at(6_000, true, false), 30_000);
        assert_eq!(fails_at(20_000, true, false), 30_000);
        assert_eq!(fails_at(2_001, true, false), 30_000);
        // Above thirty seconds (a raised maximum) the allowance is the limit.
        assert_eq!(fails_at(40_000, true, false), 40_000);
        // After a wait in publication every allowance is placed at the first published expiry
        // (4 s), and the poll renewed there is not counted: still never fewer than the default.
        assert_eq!(fails_at(DEFAULT_ALLOWANCE_MS, false, true), 6_000);
        assert_eq!(fails_at(DEFAULT_ALLOWANCE_MS, true, true), 34_000);
        for ms in [2_001, 6_000, 20_000, 30_000] {
            assert_eq!(fails_at(ms, true, true), 34_000);
        }
        assert_eq!(fails_at(6_000, false, true), 10_000);
        assert_eq!(fails_at(30_000, false, true), 34_000);
        // Nothing is counted for a poll the default renews too.
        let mut renewals = 0;
        count_allowance_poll(false, &mut renewals);
        assert_eq!(renewals, 0);
        for _ in 0..40 {
            count_allowance_poll(true, &mut renewals);
        }
        assert_eq!(renewals, SHARED_RENEWALS);
    }
    #[test]
    fn a_stopped_gpu_or_unpublished_job_keeps_the_deadline() {
        let mut renewals = 0;
        assert!(!renew_shared(true, retiring(Some(LIVENESS_NS)), &mut renewals));
        assert!(!renew_shared(true, retiring(None), &mut renewals));
        assert!(!renew_shared(false, true, &mut renewals));
        assert_eq!(renewals, 0);
    }
    #[test]
    fn a_long_job_beside_another_shares_the_gpu_without_retirements() {
        // Two traversal dispatches side by side: nothing retires, yet both progress.
        let mut renewals = 0;
        let long_job_beside = true;
        let shared = retiring(Some(LIVENESS_NS + 1)) || long_job_beside;
        for poll in 1..=14u8 {
            assert!(renew_shared(true, shared, &mut renewals));
            assert_eq!(renewals, poll);
        }
        // The thirty-second bound still ends a pair that never finishes.
        assert!(!renew_shared(true, true, &mut renewals));
    }
    #[test]
    fn work_behind_its_own_context_is_renewed_only_while_healthy_and_bounded() {
        let older = Some(7);
        for renewals in 0..SHARED_RENEWALS {
            assert!(behind_own_head(true, older, true, false, renewals));
        }
        assert!(!behind_own_head(true, older, true, false, SHARED_RENEWALS));
        assert!(!behind_own_head(true, older, true, false, u8::MAX));
        assert!(!behind_own_head(true, older, true, true, 0));
        assert!(!behind_own_head(true, None, true, false, 0));
        assert!(!behind_own_head(true, older, false, false, 0));
        assert!(!behind_own_head(false, older, true, false, 0));
    }
    #[test]
    fn exhausted_or_invalid_credit_never_wraps() {
        for value in 4..=u8::MAX {
            let mut renewals = value;
            assert!(!renew_execution(true, true, &mut renewals));
            assert_eq!(renewals, value);
        }
    }
    #[test]
    fn earlier_other_vm_gets_bounded_grace() {
        let own = head(5, 101, false);
        for count in 0..MAX_RENEWALS {
            assert_eq!(classify(own, count, [head(7, 100, false)].into_iter()), Decision::Defer);
        }
        assert_eq!(classify(own, MAX_RENEWALS, [head(7, 100, false)].into_iter()), Decision::Spare);
        assert_eq!(classify(own, 2, [].into_iter()), Decision::Own);
    }
    #[test]
    fn oldest_unfinished_queue_is_never_excused() {
        assert_eq!(classify(head(7, 100, false), 0, [head(5, 101, false)].into_iter()), Decision::Own);
    }
    #[test]
    fn a_blocker_that_already_failed_defers_then_never_spares() {
        // An innocent frame published before the culprit timed out first and failed; its
        // quarantined pair keeps its head until the firmware drains it.
        let culprit = head(7, 101, false);
        for count in 0..MAX_RENEWALS {
            assert_eq!(classify(culprit, count, [failed(5, 100)].into_iter()), Decision::Defer);
        }
        assert_eq!(classify(culprit, MAX_RENEWALS, [failed(5, 100)].into_iter()), Decision::Own);
        assert_eq!(classify(culprit, u8::MAX, [failed(5, 100)].into_iter()), Decision::Own);
        // A pair that never drains does not spare any later timeout either.
        let drained_never = [failed(5, 100), failed(5, 102)];
        for own in [head(7, 105, false), head(9, 200, false)] {
            assert_eq!(classify(own, MAX_RENEWALS, drained_never.into_iter()), Decision::Own);
        }
    }
    #[test]
    fn a_live_blocker_still_spares_beside_a_failed_one() {
        let own = head(9, 103, false);
        let others = [failed(5, 100), head(7, 101, false), failed(8, 102)];
        assert_eq!(blocker(own, others.into_iter()), Some(head(7, 101, false)));
        assert_eq!(classify(own, MAX_RENEWALS, others.into_iter()), Decision::Spare);
        // Failed or not, a blocker must be older, unfinished and of another VM.
        let late = [failed(5, 104), failed(9, 100), Head { finished: true, ..failed(5, 100) }];
        assert_eq!(blocker(own, late.into_iter()), None);
        assert_eq!(classify(own, 0, late.into_iter()), Decision::Own);
        // Among failed blockers the oldest is reported.
        let both_failed = [failed(8, 102), failed(5, 100)];
        assert_eq!(blocker(own, both_failed.into_iter()), Some(failed(5, 100)));
    }
    #[test]
    fn the_watchdog_line_names_the_blocker_at_the_first_deferral_and_the_end() {
        let own = head(9, 103, false);
        let verdict = attribute(own, 0, FRESH, [failed(5, 100)].into_iter());
        let expected = Verdict { decision: Decision::Defer, own, blocker: Some(failed(5, 100)) };
        assert_eq!(verdict, expected);
        assert_eq!(verdict.reported_blocker(0, FRESH), Some(failed(5, 100)));
        let renewed = attribute(own, 1, FRESH, [failed(5, 100)].into_iter());
        assert_eq!(renewed.reported_blocker(1, FRESH), None);
        let last = attribute(own, MAX_RENEWALS, FRESH, [failed(5, 100)].into_iter());
        assert_eq!(last.decision, Decision::Own);
        assert_eq!(last.reported_blocker(MAX_RENEWALS, FRESH), Some(failed(5, 100)));
        let spared = attribute(own, MAX_RENEWALS, FRESH, [head(5, 100, false)].into_iter());
        assert_eq!(spared.decision, Decision::Spare);
        assert_eq!(spared.reported_blocker(MAX_RENEWALS, FRESH), Some(head(5, 100, false)));
        // Nothing blocks: nothing to report.
        assert_eq!(attribute(own, 0, FRESH, [].into_iter()).reported_blocker(0, FRESH), None);
        // An excuse is named at its first poll, and so is the first counted deferral after it.
        let blocker = excused(5, 100);
        let first = attribute(own, 0, FRESH, [blocker].into_iter());
        assert_eq!(first.decision, Decision::Excuse);
        assert_eq!(first.reported_blocker(0, FRESH), Some(blocker));
        let later = Excuse { used: 3, ..FRESH };
        let excused_again = attribute(own, 0, later, [blocker].into_iter());
        assert_eq!(excused_again.reported_blocker(0, later), None);
        let past = attribute(own, 0, later, [head(5, 100, false)].into_iter());
        assert_eq!(past.decision, Decision::Defer);
        assert_eq!(past.reported_blocker(0, later), Some(head(5, 100, false)));
    }
    #[test]
    fn the_excuse_is_bounded_by_the_longest_allowance() {
        assert_eq!(excuse_limit(30_000), 15);
        assert_eq!(excuse_limit(29_000), 15);
        assert_eq!(excuse_limit(0), 1);
        // The default maximum gives 15 + 8 renewals behind other VMs' live work: 46 s of them.
        assert_eq!(POLL_MS * (excuse_limit(30_000) + u32::from(MAX_RENEWALS)), 46_000);
    }
    #[test]
    fn blockers_taking_turns_do_not_extend_the_excuse_or_the_renewals() {
        // Two other clients' commands, each inside its allowance, are ahead by turns for
        // longer than the excuse lasts. The counters belong to the waiting job: it is renewed
        // limit + MAX_RENEWALS times in all, whichever blocker is ahead, and then spared.
        let own = head(9, 103, false);
        let (mut renewals, mut excuse, mut polls) = (0u8, FRESH, 0u32);
        loop {
            let ahead = if polls % 2 == 0 { excused(5, 100) } else { excused(6, 101) };
            match attribute(own, renewals, excuse, [ahead].into_iter()).decision {
                Decision::Excuse => excuse.used += 1,
                Decision::Defer => renewals += 1,
                decision => {
                    assert_eq!(decision, Decision::Spare);
                    break;
                }
            }
            polls += 1;
            assert!(polls < 100);
        }
        assert_eq!(excuse.used, FRESH.limit);
        assert_eq!(renewals, MAX_RENEWALS);
        assert_eq!(polls, FRESH.limit + u32::from(MAX_RENEWALS));
        // Renewals spent before an excuse are not given back by it.
        let spent = attribute(own, 2, FRESH, [excused(5, 100)].into_iter());
        assert_eq!(spent.decision, Decision::Excuse);
        let after = Excuse { used: FRESH.limit, ..FRESH };
        assert_eq!(attribute(own, 2, after, [head(5, 100, false)].into_iter()).decision,
            Decision::Defer);
        assert_eq!(attribute(own, MAX_RENEWALS, after, [head(5, 100, false)].into_iter())
            .decision, Decision::Spare);
    }
    #[test]
    fn a_live_blocker_inside_its_allowance_defers_past_the_renewals() {
        // A compositor frame behind another client's 20 s compute command: its own renewals
        // stay untouched while the command runs inside its allowance.
        let own = head(9, 103, false);
        for used in 0..FRESH.limit {
            let excuse = Excuse { used, ..FRESH };
            assert_eq!(attribute(own, 0, excuse, [excused(5, 100)].into_iter()).decision,
                Decision::Excuse);
            // Past eight renewals from an earlier blocker too.
            assert_eq!(attribute(own, MAX_RENEWALS, excuse, [excused(5, 100)].into_iter())
                .decision, Decision::Excuse);
        }
        // The excuse is used up: the renewals apply, then the work is spared.
        let used = Excuse { used: FRESH.limit, ..FRESH };
        assert_eq!(attribute(own, 0, used, [excused(5, 100)].into_iter()).decision,
            Decision::Defer);
        assert_eq!(attribute(own, MAX_RENEWALS, used, [excused(5, 100)].into_iter()).decision,
            Decision::Spare);
    }
    #[test]
    fn a_blocker_past_its_allowance_gets_todays_decision() {
        let own = head(9, 103, false);
        let past = head(5, 100, false);
        for renewals in 0..MAX_RENEWALS {
            assert_eq!(classify(own, renewals, [past].into_iter()), Decision::Defer);
        }
        assert_eq!(classify(own, MAX_RENEWALS, [past].into_iter()), Decision::Spare);
    }
    #[test]
    fn a_failed_blocker_excuses_nothing() {
        let own = head(9, 103, false);
        let failed_in_allowance = Head { excused: true, ..failed(5, 100) };
        for renewals in 0..MAX_RENEWALS {
            assert_eq!(classify(own, renewals, [failed_in_allowance].into_iter()), Decision::Defer);
        }
        assert_eq!(classify(own, MAX_RENEWALS, [failed_in_allowance].into_iter()), Decision::Own);
        // A live blocker is chosen before a failed one, excused or not.
        let others = [failed_in_allowance, excused(7, 101)];
        assert_eq!(classify(own, MAX_RENEWALS, others.into_iter()), Decision::Excuse);
    }
    #[test]
    fn no_blocker_or_finished_work_is_own() {
        assert_eq!(classify(head(9, 103, false), 0, [].into_iter()), Decision::Own);
        // The oldest unfinished work is never excused, not even by later excused work.
        assert_eq!(classify(head(9, 100, false), 0, [excused(5, 101)].into_iter()), Decision::Own);
        assert_eq!(classify(head(9, 103, true), 0, [excused(5, 100)].into_iter()), Decision::Own);
        // Same-VM work does not excuse either.
        assert_eq!(classify(head(5, 103, false), MAX_RENEWALS, [excused(5, 100)].into_iter()),
            Decision::Own);
    }
    #[test]
    fn only_a_placed_long_allowance_has_an_end() {
        let poll = u64::from(POLL_MS) * MS;
        let mut default = Allowance::new(DEFAULT_ALLOWANCE_MS);
        assert!(!default.renew(true, 10_000 * MS));
        assert_eq!(default.end(), None);
        let mut long = Allowance::new(20_000);
        assert_eq!(long.end(), None);
        assert!(long.renew(true, 10_000 * MS));
        assert_eq!(long.end(), Some(10_000 * MS - poll + 20_000 * MS));
    }
    #[test]
    fn same_vm_finished_and_later_work_do_not_block() {
        let own = head(5, 101, false);
        assert_eq!(classify(own, 0, [head(5, 100, false), head(7, 100, true), head(7, 101, false), head(7, 102, false)].into_iter()), Decision::Own);
        assert_eq!(classify(head(5, 101, true), 0, [head(7, 100, false)].into_iter()), Decision::Own);
    }
}
