// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Physical render queue scope for an exiting logical context. A graph loan
//! remains foreign to the transaction until its exact owner is restored.

use super::RenderState;
use crate::g17::context::Context;
use kernel::sync::Arc;

impl RenderState {
    pub(in crate::g17::runtime) fn exit_scope(&self, context: &Arc<Context>) -> (u128, u128, u64) {
        let (mut own, mut foreign, mut slots) = (0, 0, 0);
        for (slot, entry) in self.entries.iter().enumerate() {
            let Some(entry) = entry else {
                continue;
            };
            let qids = self.qids_for_slot(slot as u8);
            let preparing = entry.preparing.is_some() || entry.constructing || entry.growing;
            if !preparing && entry.context.as_ref().is_some_and(|owner| Arc::ptr_eq(owner, context)) {
                own |= qids;
                slots |= 1u64 << slot;
            } else if preparing || entry.context.is_some() {
                foreign |= qids;
            }
        }
        (own, foreign, slots)
    }

    /// Slots of `context`'s pairs whose commands all ran but stay unretired until its kill.
    pub(in crate::g17::runtime) fn executed_slots(&self, context: &Arc<Context>) -> u64 {
        self.slots_where(|pair| pair.executed_for_kill(context))
    }

    /// Slots of `context`'s pairs whose guilty work stays unretired until its kill
    /// (`Pair::guilty_for_kill`).
    pub(in crate::g17::runtime) fn guilty_slots(&self, context: &Arc<Context>, generation: u64) -> u64 {
        self.slots_where(|pair| pair.guilty_for_kill(context, generation))
    }

    /// Slots of `context`'s pairs that only its kill releases (`Pair::releasable_by_kill`).
    pub(in crate::g17::runtime) fn kill_slots(&self, context: &Arc<Context>, generation: u64) -> u64 {
        self.slots_where(|pair| pair.releasable_by_kill(context, generation))
    }

    /// The owner and context of `slot`'s pair while that owner is still open, its VM failed as
    /// guilty (`Pair::failed_guilty`), and no builder or owner switch holds the slot.
    pub(in crate::g17::runtime) fn failed_open_owner(&self, slot: usize) -> Option<(u64, Arc<Context>)> {
        let entry = self.entries.get(slot)?.as_ref()?;
        let pair = entry.pair.as_deref()?;
        let open = !entry.closed && entry.preparing.is_none() && entry.pending_owner.is_none();
        (open && pair.owner() == Some(entry.reservation.owner) && pair.failed_guilty())
            .then(|| (entry.reservation.owner, pair.context().clone()))
    }

    fn slots_where(&self, matches: impl Fn(&crate::g17::queue::render::Pair) -> bool) -> u64 {
        let mut slots = 0;
        for (slot, entry) in self.entries.iter().enumerate() {
            if entry.as_ref().and_then(|entry| entry.pair.as_deref()).is_some_and(&matches) {
                slots |= 1u64 << slot;
            }
        }
        slots
    }

    /// The admission owner whose identities the pair in `slot` was built with.
    pub(in crate::g17::runtime) fn slot_owner(&self, slot: u8) -> Option<u64> {
        self.entries
            .get(usize::from(slot))
            .and_then(Option::as_ref)
            .map(|entry| entry.reservation.owner)
    }

    pub(in crate::g17::runtime) fn qids_for_slot(&self, slot: u8) -> u128 {
        self.entries
            .get(usize::from(slot))
            .and_then(Option::as_ref)
            .map_or(0, |entry| {
                entry.reservation.ids.into_iter().fold(0, |mask, id| {
                    mask | (1u128 << id.qid())
                })
            })
    }
}
