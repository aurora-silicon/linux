// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Read-only cursor and timestamp snapshots while a physical pair is borrowed.
//! Final stamps, timestamp refresh and ownership changes belong to commit.

use super::{memory, retirement, Pair};
use crate::g17::job::Packet;
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};

pub(crate) struct Observed {
    pub(super) ticket: retirement::Ticket,
    pub(super) snapshot: retirement::Snapshot,
}

impl Pair {
    /// Reject incomplete event/stamp prefixes before lending the graph. This
    /// is only a prefilter; commit repeats the final stamp and timestamp reads.
    pub(crate) fn observation_packet(
        &self,
        masks: [u64; 2],
        stamps: [u32; 2],
        state: u32,
    ) -> Result<Option<Arc<Packet>>> {
        if self.quarantined || self.prepared.is_some() || state != 0 || !self.selected(Some(masks))
        {
            return Ok(None);
        }
        let Some(front) = self.active.front() else {
            return Ok(None);
        };
        if !front.outer_published {
            return Ok(None);
        }
        let published = front.ticket.ordinal.checked_add(1).ok_or(EOVERFLOW)?;
        if !stamps
            .into_iter()
            .all(|stamp| retirement::stamp_covers(stamp, published))
        {
            return Ok(None);
        }
        Ok(Some(front.packet.clone()))
    }

    /// Output observation does not mutate tickets, generations, accounting or rings.
    /// The caller owns the device mutex and a current preparation epoch.
    pub(crate) fn observe_stamp_outputs(
        &self,
        stamps: [u32; 2],
        state: u32,
        defer: &mut impl FnMut(crate::g17::completion::Deferred) -> Result,
    ) -> Result {
        let armed = crate::g17::stamp_complete::enabled();
        let tracing = crate::g17::trace::hold_enabled();
        if !armed && !tracing { return Ok(()); }
        for active in self.active.iter() {
            if !active.outer_published { continue; }
            let ticket = &active.ticket;
            if !self.memory.owns(ticket.ordinal)
                || !Arc::ptr_eq(&active.packet.context, &self.memory.context)
            {
                if tracing {
                    crate::g17::trace::hold(crate::g17::trace::RENDER, self.qids()[1],
                        ticket.ordinal, active.packet.context.id(), active.packet.order.sequence, 8, 0);
                }
                continue;
            }
            let published = ticket.ordinal.checked_add(1).ok_or(EOVERFLOW)?;
            let covered = stamps.into_iter()
                .all(|stamp| retirement::stamp_covers(stamp, published));
            // Refresh after the stamp loads, never export a pre-stamp snapshot.
            fence(Ordering::Acquire);
            let Ok(ticks) = self.memory.pass(ticket.ordinal) else { continue; };
            let completion = &active.packet.completion;
            let healthy = state == 0 && !self.quarantined && self.prepared.is_none()
                && !active.failed && completion.stamp_eligible();
            if tracing {
                let cursors = snapshot(&self.memory, ticket.ordinal)
                    .and_then(|snapshot| retirement::observe(ticket, snapshot));
                let mut known = 0x7d; // cursors filled only for a valid snapshot
                let mut value = u32::from(covered) | (u32::from(ticks != [0, 0]) << 2)
                    | (u32::from(ticks[0] != 0 && ticks[1] > ticks[0]) << 4)
                    | (u32::from(healthy) << 5)
                    | (u32::from(completion.stamp_queued()) << 6);
                if let Ok(observation) = cursors {
                    known |= 2;
                    value |= u32::from(observation.retired) << 1;
                }
                crate::g17::trace::hold(crate::g17::trace::RENDER, self.qids()[1],
                    published, active.packet.context.id(), active.packet.order.sequence,
                    known, value);
            }
            if armed && healthy {
                let Ok(Some([start, end])) = retirement::stamp_pass(published, stamps,
                    || self.memory.pass(ticket.ordinal)) else { continue; };
                if completion.retain_stamp_work().is_err() { continue; }
                defer(crate::g17::completion::Deferred::Stamped(completion.clone(),
                    [start, start, start, end]))?;
                crate::g17::trace::complete(crate::g17::trace::RENDER, self.qids()[1],
                    ticket.ordinal as u32, &active.packet, [start, end]);
            }
        }
        Ok(())
    }

    pub(crate) fn observe_front(&self) -> Result<Observed> {
        let front = self.active.front().ok_or(EIO)?;
        if !Arc::ptr_eq(&front.packet.context, &self.memory.context) {
            return Err(EIO);
        }
        let ticket = front.ticket;
        for _ in 0..2 {
            fence(Ordering::Acquire);
            let snapshot = snapshot(&self.memory, ticket.ordinal)?;
            if retirement::observe(&ticket, snapshot).is_ok() {
                return Ok(Observed { ticket, snapshot });
            }
        }
        Err(EAGAIN)
    }
}

pub(super) fn snapshot(memory: &memory::Memory, ordinal: u64) -> Result<retirement::Snapshot> {
    if !memory.owns(ordinal) {
        return Err(EIO);
    }
    let cursors = memory.graph.cursors()?;
    // Sample the private head before the logical shared head, whose membership
    // may include other pairs. Individual completion does not require it empty.
    let _first = memory
        .graph
        .queues
        .dword(memory::JOB_LIST)?
        .load(Ordering::Relaxed);
    let _last = memory
        .graph
        .queues
        .dword(memory::JOB_LIST + 8)?
        .load(Ordering::Relaxed);
    let head = memory.context.work_head()?;
    Ok(retirement::Snapshot {
        cursors,
        job_list_empty: head == [0, memory.context.work_head_va()],
        pass: memory.pass(ordinal)?,
    })
}
