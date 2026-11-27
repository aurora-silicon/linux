// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Recovery targets for retained render graphs while firmware is halted.

use crate::g17::{recovery::Attribution, runtime::Registry, status::VmStatus};
use kernel::{prelude::*, sync::Arc};

impl Registry {
    pub(in crate::g17::runtime) fn report_render_recovery(&self, qid: u8, port: &crate::g17::port::Port) {
        pr_warn!("G17: recovery grow pending={} active={:?} reply={:?}\n",
            self.render.grow.requests.len(), self.render.grow.active, self.render.grow.reply);
        for entry in self.render.entries.iter().flatten().filter(|entry| {
            entry.reservation.ids.iter().any(|id| id.qid() == qid)
        }) {
            if let Some(pair) = &entry.pair {
                let qids = pair.qids();
                pair.report_recovery((|| Ok([port.stamp(qids[0])?, port.stamp(qids[1])?]))());
            } else {
                pr_warn!("G17: recovery render queue={} unavailable borrowed={} constructing={}\n",
                    qid, entry.borrowed_active, entry.constructing);
            }
        }
    }

    /// See `recovery::Queues::note_kill_sightings`.
    pub(in crate::g17::runtime) fn note_render_kill_sightings(&self, generation: u64) {
        for pair in self.render.entries.iter().flatten().filter_map(|entry| entry.pair.as_ref()) {
            pair.note_kill_sighting(generation);
        }
    }

    pub(in crate::g17::runtime) fn render_pass_started(&self, qid: u8, stamp: u64) -> Option<bool> {
        self.render.entries.iter().flatten()
            .filter_map(|entry| entry.pair.as_ref())
            .find(|pair| pair.qids().contains(&qid))?
            .pass_started(stamp)
    }

    /// See `recovery::Queues::render_culprit`.
    pub(in crate::g17::runtime) fn render_culprit(&self, blamed: u8) -> Option<Attribution> {
        let pairs = || self.render.entries.iter().flatten().filter_map(|entry| entry.pair.as_ref());
        let untouched = pairs().find(|pair| pair.qids().contains(&blamed))?.front_untouched() == Some(true);
        let running = pairs().filter_map(|pair| {
            let qids = pair.qids().into_iter().fold(0, |mask, qid| mask | 1u128 << qid);
            let (owner, start) = pair.running_start()?;
            Some((qids, owner, start))
        });
        culprit(running, untouched, |culprit| {
            let mut foreign = false;
            for queue in self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()) {
                queue.visit_owners(&mut |owner| foreign |= !Arc::ptr_eq(owner, culprit));
            }
            foreign
        })
    }

    /// See `recovery::Queues::fault_culprit`.
    pub(in crate::g17::runtime) fn fault_culprit(&self, slots: u64) -> Option<u128> {
        crate::g17::recovery::fault_qids(slots, |visit| {
            for pair in self.render.entries.iter().flatten().filter_map(|entry| entry.pair.as_ref()) {
                let qids = pair.qids().into_iter().fold(0, |mask, qid| mask | 1u128 << qid);
                pair.visit_contexts(&mut |context| visit(qids, context));
            }
            for queue in self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()) {
                let qid = 1u128 << queue.qid();
                queue.visit_contexts(&mut |context| visit(qid, context));
            }
        })
    }

    pub(in crate::g17::runtime) fn publish_render_recovery_targets(&self) -> Option<Error> {
        let mut failure = None;
        for pair in self.render.entries.iter().flatten()
            .filter_map(|entry| entry.pair.as_ref())
        {
            if let Err(error) = pair.publish_recovery_targets() {
                failure = failure.or(Some(error));
            }
        }
        failure
    }
}

/// Picks the QIDs to blame from each pair's running render: its QIDs, owner and tiler start. A VM
/// an earlier recovery found guilty comes first, even when the blamed render had started: its
/// groups queued before that keep hanging the GPU one per restart, while bystanders' renders whose
/// tiler started wait behind them, and that recovery failed its interrupted work.
///
/// Otherwise the running render whose tiler started first is the hang: a later one, the blamed
/// one included, started only after it and waits behind it (2026-11-24 01:26 UTC, m4.183: the
/// firmware blamed quickshell twice behind a spinning deqp-vk, whose render was the oldest
/// running one in every recovery). Its VM's running pairs are suspected (`Suspects`), which is
/// not proof: a slow innocent render that started first looks the same, so `classify` treats it
/// like the firmware's blame, collateral once. A blamed render that never started (`untouched`)
/// is never the hang: without a running render, or when `foreign` finds another VM's compute
/// work, which these records cannot see, nobody is named (`NoCulprit`). A started blamed render
/// beside foreign compute keeps the firmware's blame (`None`).
fn culprit<'a>(
    running: impl Iterator<Item = (u128, &'a Arc<VmStatus>, u64)> + Clone,
    untouched: bool,
    foreign: impl FnOnce(&Arc<VmStatus>) -> bool,
) -> Option<Attribution> {
    let (mut guilty, mut oldest) = (0u128, None::<(&Arc<VmStatus>, u64)>);
    for (qids, owner, start) in running.clone() {
        if owner.recovery_guilty() {
            guilty |= qids;
        }
        if oldest.is_none_or(|(_, first)| start < first) {
            oldest = Some((owner, start));
        }
    }
    if guilty != 0 {
        return Some(Attribution::Queues(guilty));
    }
    let unnamed = untouched.then_some(Attribution::NoCulprit);
    let Some((culprit, _)) = oldest else {
        return unnamed;
    };
    if foreign(culprit) {
        return unnamed;
    }
    let qids = running.filter(|(_, owner, _)| Arc::ptr_eq(owner, culprit)).fold(0, |all, (qids, ..)| all | qids);
    Some(Attribution::Suspects(qids))
}

#[kernel::macros::kunit_tests(asahi_g17_render_culprit)]
mod tests {
    use super::*;

    fn vm() -> Arc<VmStatus> {
        Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap()
    }

    #[test]
    fn a_guilty_vm_still_running_is_blamed_before_running_bystanders() {
        let (spinner, bystander) = (vm(), vm());
        // The bystander's render started first here: earlier guilt still decides.
        let running = || [(0b11, &bystander, 10), (0b1100, &spinner, 20)].into_iter();
        spinner.note_recovery_guilty();
        assert_eq!(culprit(running(), true, |_| panic!("compute is not consulted")), Some(Attribution::Queues(0b1100)));
        assert_eq!(culprit(running(), false, |_| panic!("compute is not consulted")), Some(Attribution::Queues(0b1100)));
    }

    #[test]
    fn the_oldest_running_render_is_the_culprit() {
        // 2026-11-24 01:26 UTC (m4.183, #2504 hang row run 2, T93 fronts): in every recovery the
        // spinning deqp-vk pair (51/52) held the running render with the oldest tiler start.
        let (deqp, quickshell, hyprland) = (vm(), vm(), vm());
        let deqp_qids = (1u128 << 51) | (1u128 << 52);
        // Gen 1: the firmware blamed deqp's own started render, the only one running.
        assert_eq!(culprit([(deqp_qids, &deqp, 2583940972)].into_iter(), false, |_| false), Some(Attribution::Suspects(deqp_qids)));
        // Gen 2: it blamed quickshell's render, whose tiler started after deqp's.
        let gen2 = [(deqp_qids, &deqp, 2585948130), (3 << 42, &quickshell, 2587035310)];
        assert_eq!(culprit(gen2.into_iter(), false, |_| false), Some(Attribution::Suspects(deqp_qids)));
        // Gen 3: it blamed quickshell's next render, which never started; Hyprland's started after
        // deqp's by 5858 ticks. T94 failed quickshell's VM here for the repeat.
        let gen3 = [(3 << 36, &hyprland, 2589025577), (deqp_qids, &deqp, 2589019719)];
        assert_eq!(culprit(gen3.into_iter(), true, |_| false), Some(Attribution::Suspects(deqp_qids)));
    }

    #[test]
    fn an_unstarted_render_is_never_the_culprit() {
        let (a, b) = (vm(), vm());
        // Nothing runs, or other VMs' compute could be the hang: nobody is blamed.
        assert_eq!(culprit(core::iter::empty(), true, |_| false), Some(Attribution::NoCulprit));
        assert_eq!(culprit([(0b11, &a, 5)].into_iter(), true, |_| true), Some(Attribution::NoCulprit));
        // A started blamed render beside foreign compute keeps the firmware's blame.
        assert_eq!(culprit([(0b11, &a, 5)].into_iter(), false, |_| true), None);
        assert_eq!(culprit(core::iter::empty(), false, |_| false), None);
        // All running pairs of the oldest's VM are named, none of another VM.
        let running = [(0b11, &a, 5), (0b1100, &b, 3), (0b110000, &b, 9)];
        assert_eq!(culprit(running.into_iter(), true, |_| false), Some(Attribution::Suspects(0b111100)));
    }
}
