// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Process-exit controls for closed contexts whose compute commands remain owned.

use super::{now_ns, DeferredBatch};
use crate::g17::{
    channel::Rings,
    context::Context,
    fw::channels::{ContextKill, ControlRecord},
    queue::compute::exit,
    recovery::Memory as _,
    timeout::{LIVENESS_NS, RECOVERY_WAIT_MS},
    Firmware, MSG_CONTROL_NOTIFY,
};
use core::sync::atomic::{fence, AtomicU32, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc};

/// Waits of failed contexts' kills for a moment without other contexts' compute in flight,
/// by bucket (under 10 ms, 100 ms, 1 s, 5 s, and longer) and the longest.
static KILL_WAITS: [AtomicU32; 5] = [const { AtomicU32::new(0) }; 5];
static KILL_WAIT_MAX_NS: AtomicU64 = AtomicU64::new(0);
const KILL_WAIT_BOUNDS_NS: [u64; 4] = [10_000_000, 100_000_000, 1_000_000_000, 5_000_000_000];
/// A failed context's kill waiting this long is reported, once per kill and at most once per
/// this interval across kills.
const SLOW_KILL_WAIT_NS: u64 = 5_000_000_000;
/// How often a closed context whose kill waits is looked at again ([`Firmware::kill_retry_delay`]).
const KILL_RETRY_NS: u64 = 100_000_000;
static SLOW_KILL_WARNED_NS: AtomicU64 = AtomicU64::new(0);
/// Failed contexts' kills published beside other contexts' compute under GPU memory pressure,
/// and when the last one was: at most one per [`PRESSED_KILL_INTERVAL_NS`], so the memory of
/// one is freed before another is considered.
static PRESSED_KILLS: AtomicU32 = AtomicU32::new(0);
static PRESSED_KILL_NS: AtomicU64 = AtomicU64::new(0);
const PRESSED_KILL_INTERVAL_NS: u64 = 250_000_000;

/// Render-only kills (see `Firmware::publish_render_kill`): published, acknowledged, and
/// contexts refused (each counted once, by its first reason) because the scheduler page names
/// a queue outside the context, because the held pairs have no single owner, or because
/// admission could not hold them.
static RENDER_KILLS: [AtomicU32; 5] = [const { AtomicU32::new(0) }; 5];
const RENDER_KILL_PUBLISHED: usize = 0;
const RENDER_KILL_ACKNOWLEDGED: usize = 1;
const RENDER_KILL_SCOPE: usize = 2;
const RENDER_KILL_OWNER: usize = 3;
const RENDER_KILL_HOLD: usize = 4;
/// A render-only candidate is left this long after close, as a quarantined compute queue is,
/// so that ordinary retirement goes first.
const RENDER_KILL_DELAY_NS: u64 = 250_000_000;

/// The render queue a render-only kill names: one of the context's own render queues that
/// its scheduler page lists, never the reserved queues 0 and 1.
fn render_kill_target(listed: u128, own: u128) -> Option<u8> {
    let candidates = listed & own & !0b11;
    (candidates != 0).then(|| candidates.trailing_zeros() as u8)
}

/// A closed context may be killed on its render pairs' behalf once it was never killed, no
/// compute queue still runs it, switches away from it or killed it, and a pair of it holds
/// executed but unstamped commands (`executed`) once it has been closed for
/// [`RENDER_KILL_DELAY_NS`], or holds guilty work (`guilty`, `Pair::guilty_for_kill`) at once:
/// that work never retires, it hangs the GPU again about every 83 ms, and eight such restarts
/// stop the GPU for every client. A second kill of a killed context made the firmware fault on
/// the killed pair's own tiling queue within a millisecond of acknowledging it: with the
/// pair's release held back, a context killed through its compute queue still looked like a
/// render-only candidate.
fn render_kill_eligible(age: u64, killed: bool, executed: bool, guilty: bool, compute_clear: bool) -> bool {
    !killed && compute_clear && (guilty || (executed && age >= RENDER_KILL_DELAY_NS))
}

/// A closed context's kill waits only for other contexts' compute to pause, not for their
/// render work, when its compute failed (`compute_failed`) or the kill releases render pairs
/// that nothing else releases (`releases_pairs`, `Firmware::kill_releases_render`).
fn waits_as_failed(compute_failed: bool, releases_pairs: bool) -> bool {
    compute_failed || releases_pairs
}

/// How a closed context whose render pairs are idle for a kill (`teardown_idle`) is killed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum KillRoute {
    /// Through the exit of one of its compute queues (`publish_compute_exit`).
    Compute,
    /// On its render pairs' behalf (`publish_render_kill`).
    Render,
    /// Not now.
    None,
}

/// The kill route of a closed context: whether a compute command of it failed (`failed`), its
/// render pools were all released (`pools_released`), a kill of it would release render pairs
/// whose commands all ran unstamped and no kill of it was published yet (`executed`), a compute
/// queue of it will exit (`exit_pending`), and [`render_kill_eligible`] (`render_eligible`).
///
/// A compute exit never waits for what only the kill releases. Executed pairs keep their pool
/// leases until the kill is acknowledged (`Pair::firmware_drained`): those of a failed context,
/// and those of a context whose compute commands retired without their stamps or timed out.
/// Without `executed`, the second kind waited for those pools on its compute exit, while the
/// render-only kill waited for the compute queue still holding those commands. Neither kill
/// was published, and the context, its pairs, compute queues and pools stayed held until the
/// processors stopped. With no compute queue left to exit, the render pairs carry the kill
/// instead: otherwise a quarantined pair whose last group ran but was never stamped, and whose
/// pool lease is already released, keeps the context (and its pair) held.
fn kill_route(failed: bool, pools_released: bool, executed: bool, exit_pending: bool, render_eligible: bool)
    -> KillRoute
{
    if exit_pending && (failed || pools_released || executed) {
        KillRoute::Compute
    } else if render_eligible {
        KillRoute::Render
    } else {
        KillRoute::None
    }
}

/// The one admission owner of every pair a kill holds, if there is one.
fn hold_owner(owners: impl Iterator<Item = Option<u64>>) -> Option<u64> {
    let mut owner = None;
    for slot_owner in owners {
        match (owner, slot_owner) {
            (_, None) | (_, Some(0)) => return None,
            (None, Some(next)) => owner = Some(next),
            (Some(current), Some(next)) if current != next => return None,
            _ => {}
        }
    }
    owner
}

fn refuse_render_kill(teardown: &mut crate::g17::teardown::Pending, index: usize, reason: usize) {
    if teardown.refuse_render_kill(index) {
        RENDER_KILLS[reason].fetch_add(1, Ordering::Relaxed);
    }
}

/// GPU memory is within an eighth of the device limit. A failed context holds its memory
/// until its kill is acknowledged, so kills waiting for other contexts' compute to pause can
/// push the device to its limit, and then every client's allocations fail: on Neo,
/// gnome-shell crashed on such an ENOMEM. A kill beside foreign compute risks one GPU
/// recovery instead.
fn memory_pressed() -> bool {
    let ledger = &crate::memacct::LEDGER;
    let limit = ledger.limit.load(Ordering::Relaxed);
    limit != 0
        && ledger
            .user_committed
            .load(Ordering::Relaxed)
            .saturating_add(ledger.kernel_resident.load(Ordering::Relaxed))
            > limit - limit / 8
}

fn record_kill_wait(waited: u64) {
    let bucket = KILL_WAIT_BOUNDS_NS.iter().position(|bound| waited < *bound).unwrap_or(4);
    KILL_WAITS[bucket].fetch_add(1, Ordering::Relaxed);
    KILL_WAIT_MAX_NS.fetch_max(waited, Ordering::Relaxed);
}

pub(in crate::g17) fn write_kill_waits(f: &mut kernel::fmt::Formatter<'_>, waiting: usize, longest: u64)
    -> kernel::fmt::Result
{
    let count = |bucket: usize| KILL_WAITS[bucket].load(Ordering::Relaxed);
    writeln!(f, "failed_kill_waits under_10ms={} under_100ms={} under_1s={} under_5s={} longer={} max_ms={} memory_pressed={}",
             count(0), count(1), count(2), count(3), count(4),
             KILL_WAIT_MAX_NS.load(Ordering::Relaxed) / 1_000_000, PRESSED_KILLS.load(Ordering::Relaxed))?;
    writeln!(f, "failed_kills waiting={} longest_wait_ms={}", waiting, longest / 1_000_000)
}

pub(in crate::g17) fn write_render_kills(f: &mut kernel::fmt::Formatter<'_>, waiting: usize, longest: u64)
    -> kernel::fmt::Result
{
    let count = |index: usize| RENDER_KILLS[index].load(Ordering::Relaxed);
    writeln!(f, "render_kills published={} acknowledged={} refused_scope={} refused_owner={} refused_hold={} waiting={} longest_wait_ms={}",
             count(RENDER_KILL_PUBLISHED), count(RENDER_KILL_ACKNOWLEDGED), count(RENDER_KILL_SCOPE),
             count(RENDER_KILL_OWNER), count(RENDER_KILL_HOLD), waiting, longest / 1_000_000)
}

impl super::Registry {
    /// ACK storage belongs to the already-retained transaction, so a full event
    /// batch cannot overflow a separate cache. No allocation or wait is needed.
    pub(super) fn acknowledge_compute_exit(&mut self, cookie: u64) {
        for entry in self.compute.iter_mut().flatten() {
            if let Some(mut queue) = entry.queue.as_deref_mut() {
                if queue.acknowledge_exit(cookie) {
                    return;
                }
            }
        }
        if self.teardown.acknowledge_render_kill(cookie) {
            RENDER_KILLS[RENDER_KILL_ACKNOWLEDGED].fetch_add(1, Ordering::Relaxed);
            crate::g17::trace::teardown(4, cookie, 0x100, 0);
        }
    }
}

impl Firmware {
    pub(in crate::g17) fn service_compute_exit(&mut self, deferred: &mut DeferredBatch) -> Result {
        // Released contexts' QoS rows stop naming their scheduler pages at the next event
        // turn, and at the latest before a new context is created.
        self.queues.qos_ids.forget_released(|ids| self.port.qos_forget(ids));
        let recovery_closed = self.init.recovery_state()? == 0;
        let port = &self.port;
        for entry in self.queues.compute.iter_mut().flatten() {
            if let Some(mut queue) = entry.queue.as_deref_mut() {
                queue.settle_exit(
                    recovery_closed,
                    &mut |owner| port.qos_complete(&[owner]),
                    &mut |event| deferred.push(event),
                )?;
            }
        }
        if self.recovery.pending() || !recovery_closed {
            return Ok(());
        }
        let now = now_ns();
        self.close_failed_owners(now)?;
        // Publication has already retained each kill. Retry only the doorbell;
        // failed attempts do not extend its bounded polling window.
        for entry in self.queues.compute.iter_mut().flatten() {
            if let Some(mut queue) = entry.queue.as_deref_mut() {
                let _ = queue.announce_exit(now, || self.primary.notify(MSG_CONTROL_NOTIFY));
            }
        }
        let primary = &mut self.primary;
        self.queues.teardown.announce_render_kills(|| primary.notify(MSG_CONTROL_NOTIFY));
        // Under memory pressure one kill skips the wait, at most one per interval.
        let last = PRESSED_KILL_NS.load(Ordering::Relaxed);
        let mut pressed =
            (last == 0 || now.saturating_sub(last) >= PRESSED_KILL_INTERVAL_NS) && memory_pressed();
        for index in 0..self.queues.teardown.len() {
            let Some((context, age)) = self.queues.teardown.closed(index, now) else {
                continue;
            };
            if !self.queues.render.teardown_idle(&context, self.recovery.generation())? {
                self.queues.teardown.clear_kill_wait(index);
                continue;
            }
            let result = match self.kill_route_for(index, &context, age) {
                KillRoute::Compute => {
                    // Only a render-only kill's wait lives on the entry; a compute kill keeps
                    // its own on the queue.
                    self.queues.teardown.clear_kill_wait(index);
                    self.publish_compute_exit(&context, age, now, &mut pressed)
                }
                KillRoute::Render => self.publish_render_kill(index, &context, now, &mut pressed),
                KillRoute::None => {
                    self.queues.teardown.clear_kill_wait(index);
                    Ok(())
                }
            };
            if let Err(error) = result {
                dev_err!(
                    self.primary.state.shared.dev.as_ref(),
                    "Scheduler {} exit request failed: {:?}\n",
                    context.id(),
                    error
                );
            }
        }
        // M5: a command the watchdog failed is killed once its quarantine delay has passed,
        // also while its owner keeps the queue open (`Queue::open_timeout_kill`).
        for index in 0..self.queues.compute.len() {
            let due = self.queues.compute[index]
                .as_ref()
                .and_then(|entry| entry.queue.as_deref())
                .and_then(|queue| queue.open_timeout_kill(now));
            let Some((context, age)) = due else {
                continue;
            };
            if !self.queues.render.teardown_idle(&context, self.recovery.generation())? {
                continue;
            }
            if let Err(error) = self.publish_compute_exit(&context, age, now, &mut pressed) {
                dev_err!(
                    self.primary.state.shared.dev.as_ref(),
                    "Scheduler {} exit request failed: {:?}\n",
                    context.id(),
                    error
                );
            }
        }
        Ok(())
    }

    fn publish_compute_exit(&mut self, context: &Arc<Context>, age: u64, now: u64, pressed: &mut bool) -> Result {
        let Some(index) = self.queues.compute.iter().position(|entry| {
            entry
                .as_ref()
                .and_then(|entry| entry.queue.as_deref())
                .is_some_and(|queue| queue.exit_candidate(context, age))
        }) else {
            return Ok(());
        };
        let failed = self.kill_waits_as_failed(context);
        if self.render_control_backpressured() {
            return Ok(());
        }
        let beside = failed && self.foreign_compute_running(context, now);
        if failed {
            if beside && !*pressed {
                let queue = self.queues.compute[index]
                    .as_mut()
                    .and_then(|entry| entry.queue.as_deref_mut());
                if let Some(mut queue) = queue {
                    let (waited, crossed) = queue.note_kill_wait(now, SLOW_KILL_WAIT_NS);
                    let last = SLOW_KILL_WARNED_NS.load(Ordering::Relaxed);
                    if crossed
                        && (last == 0 || now.saturating_sub(last) >= SLOW_KILL_WAIT_NS)
                        && SLOW_KILL_WARNED_NS
                            .compare_exchange(last, now.max(1), Ordering::Relaxed, Ordering::Relaxed)
                            .is_ok()
                    {
                        dev_warn!(
                            self.primary.state.shared.dev.as_ref(),
                            "Scheduler {} kill has waited {} ms for other contexts' compute to pause\n",
                            context.id(),
                            waited / 1_000_000
                        );
                    }
                }
                return Ok(());
            }
        } else if self.kill_unsafe(context, age, now) {
            return Ok(());
        }
        let (owner, target) = {
            let queue = self.queues.compute[index]
                .as_ref()
                .and_then(|entry| entry.queue.as_deref())
                .ok_or(EIO)?;
            let Some(owner) = queue.exit_owner(context) else {
                return Ok(());
            };
            (owner, queue.qid())
        };
        let (mut own, mut foreign, slots) = self.queues.render.exit_scope(context);
        for entry in self.queues.compute.iter().flatten() {
            if let Some(queue) = entry.queue.as_deref() {
                let bit = 1u128 << queue.qid();
                if queue.context().is_some_and(|bound| Arc::ptr_eq(bound, context)) {
                    own |= bit;
                } else {
                    foreign |= bit;
                }
            }
        }
        let state = exit::State::read(context)?;
        let Some(qids) = state.scope(target, own, foreign) else {
            return Ok(());
        };
        let mut hold = 0u64;
        for slot in 0..crate::hw::t8140::queues::RENDER_SLOTS as u8 {
            if slots & (1u64 << slot) != 0 && self.queues.render.qids_for_slot(slot) & qids != 0 {
                hold |= 1u64 << slot;
            }
        }
        let cookie = exit::next_cookie()?;
        if hold != 0
            && !self
                .queues
                .admission
                .hold_render_slots_for_exit_kill(owner, hold, self.queues.render.kill_slots(context, self.recovery.generation()) & hold)
        {
            return Ok(());
        }
        let record = ControlRecord::ContextKill(ContextKill::new(context.scheduler_va(), cookie));
        if let Err(error) = Rings::publish_control(&self.init, &record) {
            self.queues.admission.release_exit_kill_hold(hold);
            return if error == EAGAIN { Ok(()) } else { Err(error) };
        }
        // Publication is irrevocable even if its doorbell fails, so mark the context
        // killed before anything below can fail. Keep the killed graph until processor
        // stop, and the held render identities until this context's scheduler release
        // is consumed or processors stop.
        context.note_kill_published();
        self.note_guilty_kill(context, hold, "compute exit");
        let mut queue = self.queues.compute[index]
            .as_mut()
            .and_then(|entry| entry.queue.as_deref_mut())
            .ok_or(EIO)?;
        // A wait taken under the failed context's rule ends here, also when the kill is now
        // published under the other one (the pairs it would have released drained meanwhile).
        if failed || queue.kill_waiting(now).is_some() {
            record_kill_wait(queue.take_kill_wait(now));
        }
        if beside {
            *pressed = false;
            PRESSED_KILLS.fetch_add(1, Ordering::Relaxed);
            PRESSED_KILL_NS.store(now.max(1), Ordering::Relaxed);
        }
        queue.record_exit(cookie, now);
        crate::g17::trace::teardown(3, cookie, u64::from(context.id()), hold | u64::from(beside));
        fence(Ordering::SeqCst);
        if let Err(error) = queue.announce_exit(now, || self.primary.notify(MSG_CONTROL_NOTIFY)) {
            dev_warn!(
                self.primary.state.shared.dev.as_ref(),
                "Scheduler {} exit announcement failed: {:?}\n",
                context.id(),
                error
            );
        }
        Ok(())
    }

    /// [`kill_route`] for the closed context at teardown `index`, closed `age` ago.
    fn kill_route_for(&self, index: usize, context: &Arc<Context>, age: u64) -> KillRoute {
        kill_route(
            self.compute_failed(context),
            self.queues.render.teardown_pools_released(context),
            self.kill_releases_render(context),
            self.compute_exit_pending(context),
            self.render_kill_candidate(index, context, age),
        )
    }

    /// A kill of `context` would release render pairs of it whose commands all ran but were
    /// never stamped (`Pair::executed_for_kill`) or that hold guilty work
    /// (`Pair::guilty_for_kill`), and no kill of it was published yet. Only that kill releases
    /// them. Never a second time: a second kill of a killed context made the firmware fault on
    /// the killed pair's own tiling queue (see `render_kill_eligible`). Not on M5, which never
    /// kills a context on its render pairs' behalf.
    fn kill_releases_render(&self, context: &Arc<Context>) -> bool {
        !self.init.is_m5() && !context.kill_published() && self.queues.render.kill_slots(context, self.recovery.generation()) != 0
    }

    /// Closes, on its client's behalf, each queue whose VM a GPU recovery failed as guilty
    /// (`Pair::failed_guilty`), as its client's close (`release_queue`) would. Nothing of that
    /// VM can run again: its status refuses every later submission and publication. But a client
    /// that keeps the queue open after the lost device (a process that ignores it, or waits in an
    /// error dialog) would keep its guilty work published, and that work hangs the GPU again after
    /// every restart until the storm limit stops the GPU for every client: a guilty pair is killed
    /// only once its owner closed (`Pair::guilty_unretired`). Closed here, its context takes the
    /// same kill, release and detach as an exited client's. The client's own close later finds the
    /// queue closed (`Context::note_closed`) and leaves the teardown entry alone; the scheduler
    /// release waits for that close (`Context::held_by_open_client`). Not on M5, which
    /// takes published work back through its recovery target.
    fn close_failed_owners(&mut self, now: u64) -> Result {
        if self.init.is_m5() {
            return Ok(());
        }
        let generation = self.recovery.generation();
        for slot in 0..crate::hw::t8140::queues::RENDER_SLOTS {
            let Some((owner, context)) = self.queues.render.failed_open_owner(slot) else {
                continue;
            };
            if context.is_closed() {
                continue;
            }
            // In the order of the client's own close (`release_queue`): the context counts as
            // closed only once its teardown entry is reserved. A close that could not reserve it
            // closed nothing, and the next service pass retries it.
            if let Err(error) = self.queues.teardown.close(&context, now) {
                pr_err!("G17: scheduler {} close for its open client failed: {:?}\n", context.id(), error);
                continue;
            }
            context.note_closed(generation);
            context.note_open_client(true);
            // Closed now, so no later close releases the owner: an error from here fails the
            // event service, which stops the device, as the client's own close does.
            self.queues.release_compute(owner)?;
            self.queues.release_render(owner)?;
            // As at a client's close: hold the owner's pairs before admission releases it.
            self.queues.sync_render_holds();
            self.queues.admission.release_owner(owner);
            pr_warn!("G17: scheduler {} closed for its open client at recovery gen {}: a GPU recovery failed its VM as guilty\n",
                context.id(), generation);
        }
        Ok(())
    }

    /// Logs a published kill that releases guilty pairs (`Pair::guilty_for_kill`), with the
    /// recovery generation, so a test shows whether the next restart dropped their work.
    fn note_guilty_kill(&self, context: &Arc<Context>, hold: u64, route: &str) {
        let generation = self.recovery.generation();
        let guilty = self.queues.render.guilty_slots(context, generation) & hold;
        if guilty != 0 {
            pr_warn!("G17: scheduler {} killed by its {} at recovery gen {}: render slots {:#x} hold guilty work\n",
                context.id(), route, generation, guilty);
        }
    }

    /// A compute exit of `context` waits only for other contexts' compute to pause
    /// ([`Self::foreign_compute_running`]), as a failed context's kill and a render-only kill
    /// do, instead of for all of their work ([`Self::kill_unsafe`]). That holds when its compute
    /// failed ([`Self::compute_failed`]), and when the kill releases executed or guilty render
    /// pairs: those are what a render-only kill releases, and nothing else does. `kill_unsafe`
    /// also waits for other clients' render work, and it gives no warning and has no
    /// memory-pressure escape. Under a steady render load the pairs, pools and memory would
    /// stay held, and a guilty pair would hang the GPU until the restart storm limit.
    fn kill_waits_as_failed(&self, context: &Arc<Context>) -> bool {
        waits_as_failed(self.compute_failed(context), self.kill_releases_render(context))
    }

    /// A closed context whose compute side is clean, with render pairs whose commands all ran
    /// but were never stamped (`Pair::executed_for_kill`). Its failed compute command was
    /// retired on the firmware's fault record, so no compute queue is left to carry a kill,
    /// yet those pairs are released only after one: without this the context, its pairs and
    /// its memory stay until the processors stop. Likewise a closed context with a pair holding
    /// guilty work (`Pair::guilty_for_kill`), which only a kill stops hanging the GPU.
    ///
    /// M5 has none: it issues a context kill only from a compute queue's exit, so a
    /// render-only M5 context is never killed.
    fn render_kill_candidate(&self, index: usize, context: &Arc<Context>, age: u64) -> bool {
        !self.init.is_m5() && render_kill_eligible(
            age,
            self.queues.teardown.render_kill_published(index) || context.kill_published(),
            self.queues.render.executed_slots(context) != 0,
            self.queues.render.guilty_slots(context, self.recovery.generation()) != 0,
            self.queues.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()).all(|queue| {
                queue.idle_for_context(context) && !queue.switching_from(context) && !queue.exit_published(context)
            }),
        )
    }

    /// Kill a render-only candidate (`render_kill_candidate`). The rules of a failed
    /// context's kill apply: the pairs are held for the kill and their memory is released only
    /// once the scheduler page reports it, the kill waits for a moment without other contexts'
    /// compute unless GPU memory is pressed, and it names only queues of this context.
    fn publish_render_kill(&mut self, index: usize, context: &Arc<Context>, now: u64, pressed: &mut bool) -> Result {
        if self.render_control_backpressured() {
            return Ok(());
        }
        let beside = self.foreign_compute_running(context, now);
        if beside && !*pressed {
            let (waited, crossed) = self.queues.teardown.note_kill_wait(index, now, SLOW_KILL_WAIT_NS);
            let last = SLOW_KILL_WARNED_NS.load(Ordering::Relaxed);
            if crossed
                && (last == 0 || now.saturating_sub(last) >= SLOW_KILL_WAIT_NS)
                && SLOW_KILL_WARNED_NS
                    .compare_exchange(last, now.max(1), Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
            {
                dev_warn!(
                    self.primary.state.shared.dev.as_ref(),
                    "Scheduler {} kill has waited {} ms for other contexts' compute to pause\n",
                    context.id(),
                    waited / 1_000_000
                );
            }
            return Ok(());
        }
        let (mut own, mut foreign, slots) = self.queues.render.exit_scope(context);
        for entry in self.queues.compute.iter().flatten() {
            if let Some(queue) = entry.queue.as_deref() {
                let bit = 1u128 << queue.qid();
                if queue.context().is_some_and(|bound| Arc::ptr_eq(bound, context)) {
                    own |= bit;
                } else {
                    foreign |= bit;
                }
            }
        }
        let state = exit::State::read(context)?;
        let Some(qids) = render_kill_target(state.qids, own).and_then(|target| state.scope(target, own, foreign)) else {
            refuse_render_kill(&mut self.queues.teardown, index, RENDER_KILL_SCOPE);
            return Ok(());
        };
        let held = |slot: u8| slots & (1u64 << slot) != 0 && self.queues.render.qids_for_slot(slot) & qids != 0;
        let range = 0..crate::hw::t8140::queues::RENDER_SLOTS as u8;
        let hold = range.clone().filter(|slot| held(*slot)).fold(0u64, |hold, slot| hold | 1u64 << slot);
        let owner = hold_owner(range.filter(|slot| held(*slot)).map(|slot| self.queues.render.slot_owner(slot)));
        let Some(owner) = owner.filter(|_| hold != 0) else {
            refuse_render_kill(&mut self.queues.teardown, index, RENDER_KILL_OWNER);
            return Ok(());
        };
        // As for a failed context's kill: the cookie first, so that running out of cookies
        // leaves no admission hold behind.
        let cookie = exit::next_cookie()?;
        if !self
            .queues
            .admission
            .hold_render_slots_for_exit_kill(owner, hold, self.queues.render.kill_slots(context, self.recovery.generation()) & hold)
        {
            refuse_render_kill(&mut self.queues.teardown, index, RENDER_KILL_HOLD);
            return Ok(());
        }
        let record = ControlRecord::ContextKill(ContextKill::new(context.scheduler_va(), cookie));
        if let Err(error) = Rings::publish_control(&self.init, &record) {
            self.queues.admission.release_exit_kill_hold(hold);
            return if error == EAGAIN { Ok(()) } else { Err(error) };
        }
        // Publication is irrevocable, so mark the context killed before anything below
        // can fail. The held pairs stay held until this context's scheduler release is
        // consumed (`release_killed_slots` at detach).
        context.note_kill_published();
        self.note_guilty_kill(context, hold, "render kill");
        self.queues.teardown.record_render_kill(index, cookie, now)?;
        record_kill_wait(self.queues.teardown.take_kill_wait(index, now));
        if beside {
            *pressed = false;
            PRESSED_KILLS.fetch_add(1, Ordering::Relaxed);
            PRESSED_KILL_NS.store(now.max(1), Ordering::Relaxed);
        }
        RENDER_KILLS[RENDER_KILL_PUBLISHED].fetch_add(1, Ordering::Relaxed);
        crate::g17::trace::teardown(7, cookie, u64::from(context.id()), hold | u64::from(beside));
        fence(Ordering::SeqCst);
        let primary = &mut self.primary;
        self.queues.teardown.announce_render_kills(|| primary.notify(MSG_CONTROL_NOTIFY));
        Ok(())
    }

    /// Another context's compute command is in flight. On Neo, kills of failed contexts beside
    /// other clients' compute occasionally made the firmware recover the GPU (twice in ten
    /// minutes of a five-lane load with 67 faults), even with nothing freed before the kill's
    /// acknowledgement; such a kill takes the next moment without foreign compute instead.
    /// Render work is not waited for. A GPU that retired nothing for [`LIVENESS_NS`] has
    /// stopped. The wait is Neo's; M5 publishes its kills at once.
    ///
    /// A closed owner's queue whose commands all retired without their stamps runs nothing and
    /// is itself waiting for a kill (`Queue::running_commands`). It is not waited for: two
    /// closed contexts left that way would otherwise wait for each other's kill.
    fn foreign_compute_running(&self, context: &Arc<Context>, now: u64) -> bool {
        if self.init.is_m5() {
            return false;
        }
        let stalled = crate::g17::recovery::since_retired(now).is_none_or(|since| since >= LIVENESS_NS);
        !stalled
            && self.queues.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()).any(|queue| {
                queue.running_commands() && !queue.context().is_some_and(|bound| Arc::ptr_eq(bound, context))
            })
    }

    /// A compute command of `context` failed and the firmware holds commands behind it that it
    /// never runs, or, on M5, the watchdog failed one whose commands may never retire
    /// (`Queue::timed_out`). `publish_compute_exit` is only reached once `teardown_idle` found
    /// all of the context's render pairs idle; the pairs behind such a failure become idle when
    /// their commands have executed (`Pair::firmware_drained`), so by then nothing of the
    /// context runs any more.
    /// Delay to the next event pass while a closed context waits for its kill: a compute exit
    /// or a render-only kill that was not published yet, typically because other contexts'
    /// work was running ([`Self::kill_unsafe`], [`Self::foreign_compute_running`]). Without a
    /// pass after that work ends, an idle GPU would never publish the kill, and the closed
    /// context would keep its quarantined queues, pairs and memory while the next clients
    /// run out of room.
    pub(in crate::g17) fn kill_retry_delay(&self) -> Option<u64> {
        let now = now_ns();
        (0..self.queues.teardown.len())
            .any(|index| {
                self.queues.teardown.closed(index, now).is_some_and(|(context, _)| {
                    !context.kill_published()
                        && (self.compute_exit_pending(&context)
                            || self.queues.render.kill_slots(&context, self.recovery.generation()) != 0)
                })
            })
            .then_some(KILL_RETRY_NS)
    }

    /// A compute queue of `context` will exit, now or once its delay passed.
    fn compute_exit_pending(&self, context: &Arc<Context>) -> bool {
        self.queues.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()).any(|queue| {
            queue.exit_candidate(context, u64::MAX) || queue.exit_published(context)
        })
    }

    fn compute_failed(&self, context: &Arc<Context>) -> bool {
        self.queues.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()).any(|queue| {
            queue.context().is_some_and(|bound| Arc::ptr_eq(bound, context))
                && (queue.fault_pending() || queue.timed_out())
        })
    }

    /// A kill while other contexts' commands run makes the firmware restart the
    /// GPU, and repeated ones crash it; on an otherwise idle GPU it is safe. A
    /// closed context's commands left alone retire like any others once the GPU
    /// reaches them, so its queues stay quarantined and retained until then. Only
    /// a GPU that retired nothing for [`LIVENESS_NS`] has stopped, and then the
    /// kill proceeds. A context whose compute failed ([`Self::compute_failed`]),
    /// or whose kill releases executed render pairs, is not left alone, since its
    /// held commands never retire: its kill waits only for other contexts' compute
    /// ([`Self::kill_waits_as_failed`], [`Self::foreign_compute_running`]).
    ///
    /// On M5 the wait ends [`RECOVERY_WAIT_MS`] after the context closed (`age`): a closed
    /// context's command that never terminates would otherwise hold its queue, pairs and
    /// memory for as long as other clients keep the GPU busy.
    fn kill_unsafe(&self, context: &Arc<Context>, age: u64, now: u64) -> bool {
        let stalled = crate::g17::recovery::since_retired(now).is_none_or(|since| since >= LIVENESS_NS);
        if stalled || (self.init.is_m5() && age >= u64::from(RECOVERY_WAIT_MS) * 1_000_000) {
            return false;
        }
        self.queues.render.foreign_in_flight(context)
            || self
                .queues
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .any(|queue| {
                    queue.in_flight() && !queue.context().is_some_and(|bound| Arc::ptr_eq(bound, context))
                })
    }

    pub(in crate::g17) fn compute_exit_polling(&self, now: u64) -> bool {
        if self
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.exit_polling(now) || queue.open_timeout_polling(now))
        {
            return true;
        }
        if self.queues.teardown.render_kill_polling(now, exit::POLL_NS) {
            return true;
        }
        (0..self.queues.teardown.len()).any(|index| {
            self.queues
                .teardown
                .closed(index, now)
                .is_some_and(|(context, age)| {
                    age < exit::POLL_NS
                        && self
                            .queues
                            .compute
                            .iter()
                            .flatten()
                            .filter_map(|entry| entry.queue.as_deref())
                            .any(|queue| queue.exit_candidate(&context, u64::MAX))
                })
        })
    }
}

#[kernel::macros::kunit_tests(asahi_g17_render_kill_target)]
mod render_kill_target_tests {
    use super::{
        hold_owner, kill_route, render_kill_eligible, render_kill_target, waits_as_failed, KillRoute,
        RENDER_KILL_DELAY_NS,
    };

    #[test]
    fn executed_render_pairs_do_not_hold_back_their_compute_exit() {
        // Compute that retired without its stamps or timed out, beside an executed pair that
        // holds its pool: the kill goes through the compute exit.
        assert_eq!(kill_route(false, false, true, true, false), KillRoute::Compute);
        // As before: a failed context, or one whose pools are released.
        assert_eq!(kill_route(true, false, false, true, false), KillRoute::Compute);
        assert_eq!(kill_route(false, true, false, true, false), KillRoute::Compute);
        // No compute queue will exit: the render pairs carry the kill, or nothing does.
        assert_eq!(kill_route(false, true, true, false, true), KillRoute::Render);
        assert_eq!(kill_route(false, false, true, false, false), KillRoute::None);
        // A compute exit pending while the pools are held, and the kill would release no
        // executed pair (there is none, or the context was killed already): nothing, as before.
        assert_eq!(kill_route(false, false, false, true, false), KillRoute::None);
    }

    #[test]
    fn a_killed_context_is_never_killed_again() {
        assert!(render_kill_eligible(RENDER_KILL_DELAY_NS, false, true, false, true));
        assert!(!render_kill_eligible(RENDER_KILL_DELAY_NS, true, true, false, true));
        assert!(!render_kill_eligible(RENDER_KILL_DELAY_NS - 1, false, true, false, true));
        assert!(!render_kill_eligible(RENDER_KILL_DELAY_NS, false, false, false, true));
        assert!(!render_kill_eligible(RENDER_KILL_DELAY_NS, false, true, false, false));
        assert!(!render_kill_eligible(RENDER_KILL_DELAY_NS, true, false, true, true));
    }

    #[test]
    fn a_closed_guilty_context_is_killed_on_its_render_pairs_behalf_at_once() {
        // A pair holding a spinning group and never-started groups behind it: not executed,
        // guilty. Eligible without the retirement delay, and the render pairs carry the kill
        // when no compute queue will exit.
        assert!(render_kill_eligible(0, false, false, true, true));
        assert_eq!(kill_route(false, false, true, false, render_kill_eligible(0, false, false, true, true)),
                   KillRoute::Render);
        // A compute queue that will exit carries it instead, under the same rule as an
        // executed pair (`releases` = executed or guilty).
        assert_eq!(kill_route(false, false, true, true, false), KillRoute::Compute);
        // Never a second time, and not while compute of the context still runs.
        assert!(!render_kill_eligible(0, true, false, true, true));
        assert!(!render_kill_eligible(0, false, false, true, false));
    }

    #[test]
    fn only_a_kill_that_releases_pairs_skips_the_wait_for_foreign_render_work() {
        // Guilty or executed pairs, or failed compute: only foreign compute is waited for.
        assert!(waits_as_failed(false, true));
        assert!(waits_as_failed(true, false));
        // A closed context that is neither, with queued work: `kill_unsafe` as before, which
        // also waits for every foreign render.
        assert!(!waits_as_failed(false, false));
        // Its route is unchanged too: before the delay, or with only queued work, no render kill.
        assert!(!render_kill_eligible(RENDER_KILL_DELAY_NS * 4, false, false, false, true));
        assert_eq!(kill_route(false, false, false, false, false), KillRoute::None);
        assert_eq!(kill_route(false, true, false, true, false), KillRoute::Compute);
    }

    #[test]
    fn a_render_kill_holds_pairs_of_one_owner_only() {
        assert_eq!(hold_owner([Some(5), Some(5)].into_iter()), Some(5));
        assert_eq!(hold_owner([Some(5), Some(6)].into_iter()), None);
        assert_eq!(hold_owner([Some(5), None].into_iter()), None);
        assert_eq!(hold_owner([Some(0)].into_iter()), None);
        assert_eq!(hold_owner(core::iter::empty()), None);
    }

    #[test]
    fn a_render_kill_names_an_own_listed_queue_never_0_or_1() {
        // Listed by the scheduler page but not the context's own: no target.
        assert_eq!(render_kill_target(1 << 40, 1 << 41), None);
        // The reserved queues are never named.
        assert_eq!(render_kill_target(0b11, 0b11), None);
        assert_eq!(render_kill_target(0b111 | 1 << 40, 0b111 | 1 << 40), Some(2));
        assert_eq!(render_kill_target(1 << 40 | 1 << 90, 1 << 90), Some(90));
    }
}
