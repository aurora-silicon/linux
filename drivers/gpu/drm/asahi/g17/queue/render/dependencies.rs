// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Render predecessors retained while a physical pair is prepared off-lock.

use super::Pair;
use crate::g17::{
    dependency::Frontier,
    fw::kick::KickDependency,
    job::{self, Packet},
    status::VmStatus,
};
use crate::hw::t8140::queues::RENDER_DEPTH;
use kernel::{prelude::*, sync::Arc};

struct Predecessor {
    packet: Arc<Packet>,
    word: u64,
}

/// A loan of the graph cannot hide work already published on that pair. Fence
/// status remains live through the retained Packet, even while its storage is away.
pub(crate) struct Dependencies {
    owner: Option<u64>,
    entries: [Option<Predecessor>; RENDER_DEPTH as usize],
}

fn include(
    frontier: &mut Frontier,
    status: &Arc<VmStatus>,
    prefix: Option<u64>,
    packet: &Arc<Packet>,
    word: u64,
) -> Result {
    if !Arc::ptr_eq(packet.completion.status(), status) {
        return Ok(());
    }
    if !job::Order::contains(prefix, packet.order.sequence) {
        return Ok(());
    }
    frontier.include(
        prefix,
        packet.order.sequence,
        job::fence_status(&packet.completion.scheduler_fence()),
        word,
    )
}

impl Dependencies {
    pub(crate) fn contains_packet(&self, packet: &Arc<Packet>) -> bool {
        self.entries
            .iter()
            .flatten()
            .any(|entry| Arc::ptr_eq(&entry.packet, packet))
    }

    pub(crate) fn collect(
        &self,
        owner: u64,
        status: &Arc<VmStatus>,
        prefix: Option<u64>,
        frontier: &mut Frontier,
    ) -> Result {
        if self.owner != Some(owner) {
            return Ok(());
        }
        for entry in self.entries.iter().flatten() {
            include(frontier, status, prefix, &entry.packet, entry.word)?;
        }
        Ok(())
    }
}

impl Pair {
    /// The fixed tracker bounds this snapshot; no allocation or packet can be
    /// lost between capture and the registry moving the pair under the same lock.
    pub(crate) fn dependency_snapshot(&self) -> Dependencies {
        let qid = self.qids()[1];
        let mut active = self.active.iter();
        Dependencies {
            owner: self.owner,
            entries: core::array::from_fn(|_| {
                active.next().map(|entry| Predecessor {
                    packet: entry.packet.clone(),
                    word: KickDependency::new(qid, entry.kicks[1])
                        .map_or(u64::MAX, KickDependency::word),
                })
            }),
        }
    }

    pub(crate) fn collect_dependencies(
        &self,
        owner: u64,
        status: &Arc<VmStatus>,
        prefix: Option<u64>,
        frontier: &mut Frontier,
    ) -> Result {
        if self.owner != Some(owner) {
            return Ok(());
        }
        let qid = self.qids()[1];
        for entry in self.active.iter() {
            let word =
                KickDependency::new(qid, entry.kicks[1]).map_or(u64::MAX, KickDependency::word);
            include(frontier, status, prefix, &entry.packet, word)?;
        }
        Ok(())
    }
}
