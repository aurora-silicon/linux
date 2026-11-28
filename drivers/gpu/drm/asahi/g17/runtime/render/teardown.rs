// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Exact logical ownership gates for scheduler release and physical-pair parking.

use super::RenderState;
use crate::g17::{
    buffer::BufferIds,
    context::Context,
    initdata::InitData,
    runtime::{teardown::ReclaimBatch, Registry},
};
use kernel::{prelude::*, sync::Arc};

impl RenderState {
    pub(in crate::g17::runtime) fn observe_teardown_releases(&mut self, init: &InitData) -> Result {
        self.pools.observe(init)
    }

    /// Nothing of `context` still runs on its render pairs, so a kill of it may be published.
    /// A pair whose commands all ran unstamped (`Pair::executed_for_kill`) counts as idle: only
    /// that kill releases it. So does a pair holding guilty work (`Pair::guilty_for_kill`): it
    /// runs only to hang the GPU again, and only the kill ends that.
    pub(in crate::g17::runtime) fn teardown_idle(&self, context: &Arc<Context>, generation: u64) -> Result<bool> {
        self.idle_for(context, Some(generation))
    }

    /// As [`Self::teardown_idle`], for releasing `context`'s scheduler: a pair whose commands ran
    /// unstamped counts only once a kill of the context has been published, since until then
    /// the firmware still links that work. A killed guilty pair counts once its groups went on
    /// the guilty proof (`Pair::guilty_released`), whether or not its rings were consumed.
    pub(in crate::g17::runtime) fn release_idle(&self, context: &Arc<Context>) -> Result<bool> {
        self.idle_for(context, None)
    }

    /// `kill`: the host's recovery generation for a kill decision, `None` for the scheduler
    /// release. See [`counts_idle`].
    fn idle_for(&self, context: &Arc<Context>, kill: Option<u64>) -> Result<bool> {
        // A pair that changed owner keeps running this scheduler's queues until
        // the new owner's first command leaves it, whichever context owns it now.
        let scheduler = Some(context.scheduler_va());
        if self.entries.iter().flatten().any(|entry| match entry.pair.as_ref() {
            Some(pair) => pair.switch_in_flight() == scheduler,
            None => (entry.preparing.is_some() || entry.growing) && entry.borrowed_switch == scheduler,
        }) {
            return Ok(false);
        }
        for entry in self.entries.iter().flatten() {
            if !entry
                .context
                .as_ref()
                .is_some_and(|owner| Arc::ptr_eq(owner, context))
            {
                continue;
            }
            if entry.preparing.is_some() || entry.growing {
                return Ok(false);
            }
            if let Some(pair) = entry.pair.as_ref() {
                let idle = pair.idle_for_context(context)?;
                let guilty = kill.is_some_and(|generation| pair.guilty_for_kill(context, generation));
                if !counts_idle(kill.is_none(), context.kill_published(), idle, pair.executed_for_kill(context),
                                guilty, pair.guilty_released())
                {
                    return Ok(false);
                }
            }
        }
        Ok(true)
    }

    /// Some render pair runs another context's commands.
    pub(in crate::g17::runtime) fn foreign_in_flight(&self, context: &Arc<Context>) -> bool {
        self.entries.iter().flatten().any(|entry| {
            !entry.context.as_ref().is_some_and(|owner| Arc::ptr_eq(owner, context))
                && entry.pair.as_ref().is_some_and(|pair| pair.in_flight())
        })
    }

    pub(in crate::g17::runtime) fn teardown_pools_released(&self, context: &Arc<Context>) -> bool {
        context.render_pools_released(|id, control| self.pools.released(id, control))
            && self
                .retired
                .iter()
                .filter(|old| Arc::ptr_eq(old.context(), context))
                .all(|old| {
                    self.pools
                        .released(u32::from(old.pool().id()), old.pool().control_va())
                })
    }

    /// Warm superseded roots need only their exact executed replacement witness.
    /// Graphs with their own memory remain until that Context's scheduler ack.
    pub(in crate::g17::runtime) fn collect_reclaimable(
        &mut self,
        out: &mut ReclaimBatch,
    ) -> Result {
        for slot in 0..self.entries.len() {
            self.collect_retired(slot)?;
        }
        let mut index = 0;
        while index < self.retired.len() {
            // An M5 pool replaced by a same-context rebind stays until
            // firmware acknowledged its release.
            let pool = self.retired[index].pool();
            if self.retired[index].buffer_id().is_some()
                || (self.retired[index].wait_for_pool_release()
                    && !self.pools.released(u32::from(pool.id()), pool.control_va()))
            {
                index += 1;
                continue;
            }
            let Ok(target) = out.binding_slot() else {
                break;
            };
            *target = Some(self.retired.remove(index).map_err(|_| EIO)?);
        }
        Ok(())
    }

    /// Preflight the entire Context before detaching its first graph. Successor
    /// graphs on the same VM have a different Arc and remain installed. Returns the slots
    /// whose killed guilty pairs were kept out of service (`Pair::can_keep`): their kill
    /// hold stays.
    pub(in crate::g17::runtime) fn detach_context(
        &mut self,
        context: &Arc<Context>,
        buffers: &mut BufferIds,
        out: &mut ReclaimBatch,
    ) -> Result<u64> {
        let mut count = self
            .retired
            .iter()
            .filter(|old| Arc::ptr_eq(old.context(), context))
            .count();
        for entry in self.entries.iter().flatten() {
            if !entry
                .context
                .as_ref()
                .is_some_and(|owner| Arc::ptr_eq(owner, context))
            {
                continue;
            }
            if entry.preparing.is_some() || entry.growing || entry.parked.is_some() {
                return Err(EBUSY);
            }
            if let Some(pair) = entry.pair.as_ref() {
                if !pair.can_park(context)? && !pair.can_keep(context) {
                    return Err(EBUSY);
                }
                count += 1;
            }
        }
        if !out.has_room(count, 1) {
            return Err(EBUSY);
        }
        let linked = context.firmware_may_link();
        let mut kept = 0u64;
        for (slot, entry) in self.entries.iter_mut().enumerate() {
            let Some(entry) = entry.as_mut() else {
                continue;
            };
            if !entry
                .context
                .as_ref()
                .is_some_and(|owner| Arc::ptr_eq(owner, context))
            {
                continue;
            }
            let Some(pair) = entry.pair.as_ref() else {
                continue;
            };
            // Either end state of a killed guilty pair is logged: the test of the kill fix
            // reads from it whether the firmware consumed the unlinked groups' ring entries.
            let keep = !pair.can_park(context)?;
            if pair.guilty_released() {
                pr_warn!("G17: render slot {}: killed guilty pair {} (cursors {:?})\n", slot,
                    if keep { "kept out of service until the GPU stops: its rings were not consumed" }
                    else { "parked: its rings were consumed" },
                    pair.ring_cursors());
            }
            buffers.unreserve_owner(pair.buffer_id())?;
            let target = out.binding_slot()?;
            let pair = entry.pair.take().ok_or(EIO)?;
            let (mut parked, retired) = KBox::into_inner(pair).park();
            if keep {
                // Neither the rings nor the graph their entries name are reused or freed, and
                // the slot's kill hold stays, until the processors stop.
                keep_graph(&mut self.kept_graphs, retired);
                keep_pair(&mut self.kept_pairs, parked);
                kept |= 1u64 << slot;
                entry.context = None;
                entry.pool = None;
                continue;
            }
            // This park follows the consumed release of `context`.
            parked.release_consumed(context.scheduler_va());
            if linked {
                // Guard (kill before free): the firmware may still walk this context's work
                // list into the graph, so it is not freed or recycled before the firmware stops.
                kernel::pr_warn_once!("G17: keeping render graphs of an unkilled context that still links work\n");
                keep_graph(&mut self.kept_graphs, retired);
            } else {
                *target = Some(retired);
            }
            entry.parked = Some(parked);
            // Detached backing retains these references until the worker runs.
            entry.context = None;
            entry.pool = None;
        }
        let mut index = 0;
        while index < self.retired.len() {
            if !Arc::ptr_eq(self.retired[index].context(), context) {
                index += 1;
                continue;
            }
            if let Some(id) = self.retired[index].buffer_id() {
                buffers.unreserve_owner(id)?;
            }
            let retired = self.retired.remove(index).map_err(|_| EIO)?;
            if linked {
                keep_graph(&mut self.kept_graphs, retired);
            } else {
                *out.binding_slot()? = Some(retired);
            }
        }
        Ok(kept)
    }
}

impl Registry {
    /// Close while a builder owns a pair records intent. Its return path applies
    /// the release before admitting further commands from either logical owner.
    pub(in crate::g17::runtime) fn release_render(&mut self, owner: u64) -> Result {
        for entry in self.render.entries.iter_mut().flatten() {
            if entry.pending_owner == Some(owner) {
                entry.pending_owner_closed = true;
            }
            if entry.reservation.owner != owner {
                continue;
            }
            entry.closed = true;
            if let Some(pair) = entry.pair.as_mut() {
                pair.release_owner();
            }
        }
        Ok(())
    }
}

/// Whether a render pair of a closed context counts as idle: for the kill decision
/// (`release` false) or for the scheduler release, which lets the pair's ring and graph go.
/// - `idle`: nothing of it runs (`Pair::idle_for_context`).
/// - `executed`: its commands all ran unstamped (`Pair::executed_for_kill`); for the release
///   only once the kill was published (`kill_published`).
/// - `guilty`: it holds guilty work (`Pair::guilty_for_kill`); for the kill decision only. Its
///   started group may run until the restart after the kill, so counting it for the release
///   would free a running group's ring.
/// - `guilty_released`: its groups went on the guilty proof (`Pair::guilty_released`); for the
///   release, its rings may never be consumed.
fn counts_idle(release: bool, kill_published: bool, idle: bool, executed: bool, guilty: bool, guilty_released: bool)
    -> bool
{
    idle
        || (executed && (!release || kill_published))
        || (guilty && !release)
        || (guilty_released && release && kill_published)
}

/// Keeps a killed guilty pair until the firmware stops (see `detach_context`); if even that
/// cannot be recorded, it is leaked rather than dropped.
fn keep_pair(kept: &mut KVec<crate::g17::queue::render::parked::ParkedPair>, pair: crate::g17::queue::render::parked::ParkedPair) {
    if kept.reserve(1, GFP_KERNEL).is_ok() {
        if let Err(error) = kept.push_within_capacity(pair) {
            LEAKED_GRAPHS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
            core::mem::forget(error.0);
        }
    } else {
        LEAKED_GRAPHS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        core::mem::forget(pair);
    }
}

#[kernel::macros::kunit_tests(asahi_g17_render_teardown_idle)]
mod idle_tests {
    use super::counts_idle;

    #[test]
    fn a_guilty_pair_never_counts_idle_for_the_scheduler_release_while_it_may_run() {
        // The use-after-free guard: killed, guilty, not idle, not yet released on the proof.
        assert!(!counts_idle(true, true, false, false, true, false));
        assert!(!counts_idle(true, false, false, false, true, false));
        // For the kill decision it counts.
        assert!(counts_idle(false, false, false, false, true, false));
        // Released on the guilty proof: counts for the release once the kill was published.
        assert!(counts_idle(true, true, false, false, false, true));
        assert!(!counts_idle(true, false, false, false, false, true));
    }

    #[test]
    fn idle_and_executed_pairs_count_as_before() {
        assert!(counts_idle(true, false, true, false, false, false));
        assert!(counts_idle(false, false, false, true, false, false));
        assert!(!counts_idle(true, false, false, true, false, false));
        assert!(counts_idle(true, true, false, true, false, false));
        assert!(!counts_idle(false, false, false, false, false, false));
    }
}

/// Graphs that could not be recorded in `kept_graphs` and were leaked instead, for debugfs.
pub(super) static LEAKED_GRAPHS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/// Keeps `graph` until the firmware stops; if even that cannot be recorded, it is leaked
/// rather than freed.
fn keep_graph(
    kept: &mut KVec<crate::g17::queue::render::RetiredBinding>,
    graph: crate::g17::queue::render::RetiredBinding,
) {
    if kept.reserve(1, GFP_KERNEL).is_ok() {
        if let Err(error) = kept.push_within_capacity(graph) {
            LEAKED_GRAPHS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
            core::mem::forget(error.0);
        }
    } else {
        LEAKED_GRAPHS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        core::mem::forget(graph);
    }
}
