// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Firmware restart handshake and per-VM failure classification.
//!
//! The device mutex serializes this state with publication. Preparations are closed and joined
//! without that mutex before `service`; no queue callback may signal a fence here. Queue
//! owners retain failed work until its retirement witness, a complete pair-drain witness, or
//! processor stop. A failed fence alone never releases mappings.

mod attribution;
mod slot;
mod safety;
pub(crate) use safety::{note_retired, since_retired};
pub(crate) use attribution::{fault_qids, Record, Sources};
pub(crate) use attribution::Attribution;

pub(super) fn slot_key(slot: usize, slot_keys: Option<u64>) -> Option<u64> {
    slot::key(slot, slot_keys?).ok()
}

use super::status::VmStatus;
use kernel::{prelude::*, sync::Arc};

const QIDS: u8 = 128;
const GUILTY: u8 = 1 << 1;
const CANCELLED: u8 = 1 << 2;
const MAX_GUILTY_VMS: usize = 8;
/// One-millisecond ticks per handshake phase. The firmware finishes a restart only after the
/// work it interrupted has drained; with several clients running long ray-traversal jobs that
/// takes seconds, and giving up loses every client's device. Ten seconds matches the longest
/// execution allowance (`timeout::POLL_MS` plus renewals).
const RECOVERY_POLLS: usize = 10_000;

/// Records in the firmware recovery table.
pub(super) const RECOVERY_RECORDS: usize = 256;

/// Per-QID classifications from the firmware recovery table.
/// Multiple records for a queue contribute all of their classification bits.
#[derive(Default)]
pub(super) struct Entries {
    guilty: u128,
    cancelled: u128,
    /// Valid records, each naming one command that was in flight at the restart.
    pub(super) listed: u32,
    /// Listed commands found by their exact stamp, and by the oldest-work fallback.
    pub(super) exact: u32,
    pub(super) fallback: u32,
    /// QIDs with a listed command found by its exact stamp: that command was dropped.
    exact_qids: u128,
    /// QIDs with a valid record.
    queues: u128,
    /// The first records, for the recovery's log line.
    shown: Shown,
}

/// Records of a recovery table shown in its one log line.
const SHOWN: usize = 24;

/// The first [`SHOWN`] valid records of a recovery table as `q<qid>:<stamp>:<flags>/<match>`,
/// the match being how the host found the command (`E` by exact stamp, `O` as the oldest
/// published work, `-` not found, `?` not matched on M5): which queues the restart listed, and
/// whether the firmware flagged them guilty, decide what the recovery fails and whether a
/// restart storm counts it.
#[derive(Default)]
struct Shown {
    records: [(u32, u32, u8); SHOWN],
    count: usize,
    more: usize,
}

impl Shown {
    fn push(&mut self, flags: u32, stamp: u32, matched: u8) {
        match self.records.get_mut(self.count) {
            Some(record) => {
                *record = (flags, stamp, matched);
                self.count += 1;
            }
            None => self.more += 1,
        }
    }
}

impl kernel::fmt::Display for Shown {
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        for (index, &(flags, stamp, matched)) in self.records[..self.count].iter().enumerate() {
            let sep = if index == 0 { "" } else { " " };
            write!(f, "{}q{}:{:#x}:{:#x}/{}", sep, (flags >> 4) & 0x7f, stamp, flags, matched as char)?;
        }
        if self.more != 0 {
            write!(f, " +{}", self.more)?;
        }
        Ok(())
    }
}

impl Entries {
    /// Counts a valid record. Returns false for an invalid one, which ends the walk on G17P
    /// and is skipped on M5 (see `read_entries`).
    pub(super) fn include(&mut self, flags: u32) -> bool {
        if flags & 1 == 0 {
            return false;
        }
        let qid = (flags >> 4) & 0x7f;
        let bit = 1u128 << qid;
        if flags & u32::from(GUILTY) != 0 {
            self.guilty |= bit;
        }
        if flags & u32::from(CANCELLED) != 0 {
            self.cancelled |= bit;
        }
        self.queues |= bit;
        self.listed += 1;
        true
    }
}

/// How a listed recovery record was matched to retained work.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) enum Interrupted {
    /// No retained command of the queue.
    None,
    /// The command whose completion stamp equals the record's.
    Exact,
    /// No stamp matched; the queue's oldest unretired command was marked instead.
    Oldest,
}

/// How an error completion affects its VM's sticky status and other jobs.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) enum Class {
    /// Only this work's fence fails; VM status and unrelated work remain usable.
    Spared,
    /// This VM owns guilty work.
    Guilty,
}

/// Queue operations under the device mutex. Completion signalling belongs to the caller after
/// that mutex is dropped. `oldest_spared` returns None if the QID has no in-flight owner.
pub(super) trait Queues {
    fn render_scheduler_active(&self) -> bool;
    fn render_pass_started(&self, qid: u8, stamp: u64) -> Option<bool>;
    /// Read while firmware is halted, like `render_pass_started`. The QIDs of every pair of a VM
    /// an earlier recovery found guilty with a render that started and did not finish. Without
    /// one, `Suspects` the VM whose such render started first, unless another VM holds compute
    /// work, which these records cannot see. `NoCulprit` when the oldest published render of the
    /// pair holding `blamed` had not started and no VM can be named; None keeps the firmware's
    /// blame.
    fn render_culprit(&self, _blamed: u8) -> Option<Attribution> { None }
    /// Copy every retained queue's live producer to its recovery target, then
    /// issue a full barrier. A failed graph must not skip subsequent queues.
    fn publish_recovery_targets(&self) -> Option<Error>;
    fn oldest_spared(&self, qid: u8) -> Option<bool>;
    fn visit_owners(&self, qid: u8, visit: &mut dyn FnMut(&Arc<VmStatus>));
    /// Classify every in-flight completion; return QIDs of render slots with spared work.
    fn classify_work(&mut self, class: &dyn Fn(&Arc<VmStatus>) -> Class) -> u128;
    /// Mark the command a recovery record lists as interrupted. It may have
    /// partly executed, so it must never report success; see
    /// `Completion::note_interrupted`.
    fn note_interrupted(&mut self, qid: u8, stamp: u32) -> Interrupted;
    /// Optional diagnostics only; absence cannot alter attribution or release.
    fn report_blamed_render(&self, _qid: u8) {}
    /// Read while firmware is halted: a restart before `generation` blamed the spared work of the
    /// render pair that holds `qid` as collateral already, and none of that pair's work retired
    /// since (`note_collateral`). Its spared work hung the GPU again after the restart that blamed
    /// it, so the hang is its own. Never for a compute queue.
    fn collateral_before(&self, _qid: u8, _generation: u64) -> bool { false }
    /// The restart `generation` blamed the spared work of the render pair that holds `qid` as
    /// collateral. Marks that pair, not the QID: a QID is reused once its owner closes, and the
    /// next owner starts unmarked. A retirement of the pair's work clears the mark.
    fn note_collateral(&mut self, _qid: u8, _generation: u64) {}
    /// `note_collateral` keeps a mark for the queue that holds `qid`, so a repeated blame on it
    /// can be told from a first one. Only G17P render pairs keep one.
    fn marks_collateral(&self, _qid: u8) -> bool { false }
    /// Read while firmware is halted for the restart the host accepted after `generation`:
    /// every killed context whose kill the firmware finished counts as first seen done at
    /// `generation` at the latest, so this restart releases its guilty pairs
    /// (`Context::restarted_since_kill`). Without this, a kill finished just before a restart
    /// that the host had not observed yet would wait for a restart that may never come.
    fn note_kill_sightings(&self, _generation: u64) {}
    /// Logs the MMU fault banks while the firmware is halted and returns the UAT slots
    /// (context IDs) of the faulting accesses as a mask; 0 without a decodable user fault.
    fn report_mmu_fault(&self) -> u64 { 0 }
    /// Read while firmware is halted, like `render_pass_started`. The QIDs of every queue with
    /// an in-flight packet whose context is in `slots`; None if there is none.
    fn fault_culprit(&self, _slots: u64) -> Option<u128> { None }
    fn settle_blamed_render(&mut self, qid: u8, error: Error);
    fn settle_cancelled_renders(&mut self, qids: u128);
    fn rescan_compute(&mut self) -> Result;
    fn release_retired_compute(&mut self);
}

/// Live shared-memory operations. The handshake word uses aligned 32-bit accesses; transitions
/// check the expected value, issue a full barrier, store the next value, then another barrier.
pub(super) trait Memory {
    fn recovery_state(&self) -> Result<u32>;
    fn host_recovery(&self) -> Result<u32>;
    fn fault_sources(&self) -> Result<Sources>;
    fn transition(&self, expected: u32, next: u32) -> Result;
    fn clear_timestamps(&self) -> Result;
    /// One recovery record's flags and completion stamp, read while firmware
    /// is halted in state 3.
    fn entry(&self, index: usize) -> Result<(u32, u32)>;
    /// One millisecond wait; this is polled because the firmware sends no state-3 notification.
    fn wait_tick(&self);
    /// The M5 firmware's recovery table, whose stamp form is not the G17P one.
    fn is_m5(&self) -> bool;
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct Request {
    generation: u64,
    blamed: Option<u8>,
}

/// Generation accepted by the host and the coalesced request from either coprocessor.
pub(super) struct State {
    accepted: u64,
    pending: Option<Request>,
    timeouts: safety::Storm,
    restarts: safety::Storm,
}

/// Non-fatal post-recovery failures. The firmware handshake has already closed successfully.
pub(super) struct ServiceResult {
    pub(super) scan_error: Option<Error>,
    pub(super) target_error: Option<Error>,
    /// Every VM found guilty had failed before, and the restart dropped one of its commands.
    pub(super) leftover: bool,
    /// The restart found guilty VMs and is charged to them ([`VmStatus::charge_restart`]): each
    /// was found guilty for the first time, which costs its client the device, or still held
    /// work from then. It does not count toward the device's storm limit.
    pub(super) charged: bool,
}

impl State {
    pub(super) const fn new() -> Self {
        Self {
            accepted: 0,
            pending: None,
            timeouts: safety::Storm::new(),
            restarts: safety::Storm::new(),
        }
    }
    pub(super) fn timeout_limit(&mut self, now: u64) -> bool {
        self.timeouts.note_unless_progress(now, safety::retired())
    }
    /// Like timeouts, restarts with work retired between them start a new window: each one
    /// still costs the guilty client its device, but only restarts without progress stop the GPU.
    /// Restarts charged to their guilty clients (`ServiceResult::charged`) are not counted.
    pub(super) fn restart_limit(&mut self, now: u64) -> bool {
        self.restarts.note_unless_progress(now, safety::retired())
    }
    /// A restart that only dropped leftover work of failed VMs (`ServiceResult::leftover`)
    /// does not count toward the storm limit. Returns whether it was forgiven.
    pub(super) fn forgive_restart(&mut self) -> bool { self.restarts.forgive() }
    pub(super) fn generation(&self) -> u64 {
        self.accepted
    }
    pub(super) fn pending(&self) -> bool {
        self.pending.is_some()
    }

    /// Returns false for a consumed duplicate from an older generation, true when the event
    /// drain must stop until the current request is serviced. The first blamed QID wins.
    pub(super) fn request(&mut self, generation: u64, stamp: i32) -> Result<bool> {
        if generation < self.accepted {
            return Ok(false);
        }
        if generation != self.accepted {
            return Err(EIO);
        }
        if stamp != -1 && !(0..i32::from(QIDS)).contains(&stamp) {
            return Err(EIO);
        }
        let blamed = if (0..i32::from(QIDS)).contains(&stamp) {
            Some(stamp as u8)
        } else {
            None
        };
        if let Some(pending) = self.pending.as_mut() {
            if pending.generation != generation {
                return Err(EBUSY);
            }
            pending.blamed = pending.blamed.or(blamed);
        } else {
            self.pending = Some(Request { generation, blamed });
        }
        Ok(true)
    }
}

/// The firmware owner implements memory and queue access without splitting its
/// mutable borrow or duplicating the authoritative restart generation.
pub(super) trait Host: Memory + Queues {
    fn recovery(&self) -> &State;
    fn recovery_mut(&mut self) -> &mut State;
}

/// Performs one pending restart. The caller has closed preparation admission and joined all
/// builders off-lock. Work remains mapped throughout every callback, including on error.
pub(super) fn service<H: Host>(
    host: &mut H,
    crashed: impl Fn() -> bool,
) -> Result<ServiceResult> {
    let request = host.recovery().pending.ok_or(EINVAL)?;
    if request.generation != host.recovery().accepted || host.host_recovery()? != 0 {
        return Err(EIO);
    }
    // The restart record is published before firmware announces state 1.
    // Its ring cursor is already consumed; wait only for that readiness word,
    // without advancing the host generation or acknowledging an early event.
    let mut ready = false;
    for _ in 0..RECOVERY_POLLS {
        if crashed() {
            return Err(EIO);
        }
        match host.recovery_state()? {
            1 => {
                ready = true;
                break;
            }
            0 => host.wait_tick(),
            _ => return Err(EIO),
        }
    }
    if !ready {
        pr_warn!("G17: recovery gen {} did not reach state 1 (state {:?})\n",
            request.generation, host.recovery_state());
        return Err(ETIMEDOUT);
    }
    // Recheck ownership after the off-CPU waits, before any handshake mutation.
    if host.host_recovery()? != 0 {
        return Err(EIO);
    }
    host.recovery_mut().accepted = host.recovery().accepted.wrapping_add(1);
    if request.generation == 0 && !host.render_scheduler_active() {
        host.clear_timestamps()?;
    }
    let mut sources = host.fault_sources()?;
    sources.report(request.generation, super::queue::compute::retirement::stampless_total(),
        request.blamed);
    let fault_slots = host.report_mmu_fault();
    if let Some(qid) = request.blamed {
        host.report_blamed_render(qid);
    }
    // The keys, monitor and pass-start evidence must describe the same halted
    // instant. Firmware may update pass timestamps as soon as state 3 is
    // acknowledged; sampling them during classification can blame a bystander.
    let attribution = sources.attribute(request.blamed,
        |qid, stamp| host.render_pass_started(qid, stamp));
    let attribution = sources.host_fallback(attribution,
        || host.render_culprit(request.blamed?));
    let attribution = sources.fault_override(attribution, request.blamed, fault_slots,
        |slots| host.fault_culprit(slots));
    if let Attribution::Suspects(qids) = attribution {
        pr_warn!("G17: recovery gen {} suspects queues {:#x}\n", request.generation, qids);
    }
    if let Attribution::Queues(qids) = attribution {
        pr_warn!("G17: recovery gen {} blames queues {:#x}\n", request.generation, qids);
    }
    let target_error = host.publish_recovery_targets();
    host.transition(1, 2)?;
    let mut recovered = false;
    for _ in 0..RECOVERY_POLLS {
        if host.recovery_state()? == 3 {
            recovered = true;
            break;
        }
        if crashed() {
            return Err(EIO);
        }
        host.wait_tick();
    }
    if !recovered {
        pr_warn!("G17: recovery gen {} did not reach state 3 (state {:?})\n",
            request.generation, host.recovery_state());
        return Err(ETIMEDOUT);
    }
    // A failed table read leaves only the explicit blamed QID as evidence of guilt.
    // Every listed command was in flight when firmware stopped. Firmware may
    // still retire it with stamps and records that look like success, so it is
    // marked before firmware resumes and never reports success.
    let entries = read_entries(host);
    host.note_kill_sightings(host.recovery().accepted.wrapping_sub(1));
    host.transition(3, 0)?;
    // The records are copied: log them once the firmware runs again.
    pr_warn!("G17: recovery gen {} table: {} records [{}]\n", request.generation, entries.listed, entries.shown);
    if host.recovery_state()? != 0 {
        return Err(EIO);
    }
    host.recovery_mut().pending = None;
    report_clients(host, request.generation, &entries, request.blamed);

    let (listed, exact, fallback) = (entries.listed, entries.exact, entries.fallback);
    let (cancelled, blame, leftover, charged) =
        classify(host, entries, request.blamed, attribution, request.generation);
    if listed != 0 && host.is_m5() {
        // The M5 table only classifies (read_entries): each command is retired from its own
        // queue's records, replayed when it never ran.
        pr_warn!(
            "G17: GPU recovery: {} commands were in flight; each is retired from its own queue's records\n",
            listed
        );
    } else if listed != 0 {
        pr_warn!(
            "G17: GPU recovery: {} commands were in flight ({} matched by stamp, {} by queue); they fail instead of completing\n",
            listed,
            exact,
            fallback
        );
    }
    if let Some(qid) = request.blamed {
        pr_warn!(
            "G17: GPU recovery (reason {:?}): firmware blamed queue {}, {}\n",
            sources.firmware_reason(),
            qid,
            if blame { "failing its oldest render" } else { "attribution cleared it; spared" }
        );
        if blame {
            host.settle_blamed_render(qid, EIO);
        }
    }
    if cancelled != 0 {
        host.settle_cancelled_renders(cancelled);
    }
    let scan_error = host.rescan_compute().err();
    host.release_retired_compute();
    Ok(ServiceResult { scan_error, target_error, leftover, charged })
}

/// Names the client of each queue the recovery table lists, and of the blamed queue, before any
/// classification changes: every recovery then says whose work was in flight. Diagnostics only.
fn report_clients<Q: Queues>(queues: &Q, generation: u64, entries: &Entries, blamed: Option<u8>) {
    let named = entries.queues | blamed.map_or(0, |qid| 1u128 << qid);
    for qid in (0..QIDS).filter(|qid| named & (1u128 << qid) != 0) {
        let bit = 1u128 << qid;
        let mut first: Option<Arc<VmStatus>> = None;
        let (mut commands, mut others) = (0u32, 0u32);
        queues.visit_owners(qid, &mut |status| {
            commands += 1;
            match first.as_ref().map(|known| Arc::ptr_eq(known, status)) {
                None => first = Some(status.clone()),
                Some(false) => others += 1,
                Some(true) => {}
            }
        });
        let flags = (
            if blamed == Some(qid) { " blamed" } else { "" },
            if entries.guilty & bit != 0 { " guilty" } else { "" },
            if entries.cancelled & bit != 0 { " cancelled" } else { "" },
        );
        match first {
            Some(status) => {
                let (name, tgid) = status.client();
                let shared = if others != 0 { " and other clients" } else { "" };
                pr_warn!(
                    "G17: recovery gen {} queue {}{}{}{}: {} retained commands, client {}[{}]{}\n",
                    generation, qid, flags.0, flags.1, flags.2, commands, name, tgid, shared
                );
            }
            None => pr_warn!(
                "G17: recovery gen {} queue {}{}{}{}: no retained commands\n",
                generation, qid, flags.0, flags.1, flags.2
            ),
        }
    }
}

/// Walks the recovery table. G17P's is a packed list: records after its first invalid one are
/// left over from an earlier restart, so the walk ends there. M5's need not be packed, so all
/// of its records are walked and invalid ones skipped; they only classify the recovery, while
/// on G17P every listed command is marked interrupted. A failed read keeps what was read so far.
fn read_entries<H: Host>(host: &mut H) -> Entries {
    let mut entries = Entries::default();
    let m5 = host.is_m5();
    for index in 0..RECOVERY_RECORDS {
        let Ok((flags, stamp)) = host.entry(index) else {
            break;
        };
        if !entries.include(flags) {
            if m5 {
                continue;
            }
            break;
        }
        if m5 {
            entries.shown.push(flags, stamp, b'?');
            continue;
        }
        let qid = ((flags >> 4) & 0x7f) as u8;
        let matched = match host.note_interrupted(qid, stamp) {
            Interrupted::Exact => {
                entries.exact += 1;
                entries.exact_qids |= 1u128 << qid;
                b'E'
            }
            Interrupted::Oldest => {
                entries.fallback += 1;
                b'O'
            }
            Interrupted::None => b'-',
        };
        entries.shown.push(flags, stamp, matched);
    }
    entries
}

/// Resolves guilty QIDs before changing any completion's classification. Unknown ownership or
/// too many distinct guilty VMs leaves all prior classifications intact. The oldest spared work
/// of a QID, or blame that no evidence confirmed (`Attribution::Firmware`), makes that blame
/// collateral rather than new VM guilt, once:
/// the restart `generation` marks the render pair it blamed so (`Queues::note_collateral`), and
/// when a later restart blames that pair again with none of its work retired in between, the
/// hang is its own and its VM is guilty (`Queues::collateral_before`). Otherwise a first blame
/// that landed on a starved bystander would leave the real culprit's spared work immune: never
/// guilty, never poisoned, never forgiven and never killed, until the restart storm limit
/// (2026-11-19 19:12 UTC, 2026-11-20 15:03 UTC).
/// Also returns whether the firmware's blamed QID is still guilty after attribution: a blamed
/// render that had not started while others had (or with no culprit at all) is innocent, and its
/// work is spared like that of every other innocent VM instead of failing with EIO.
/// Then whether every guilty VM had failed before this restart and it dropped one of their
/// commands: a failed VM publishes nothing new, so such restarts end with its queued work.
/// Last, whether the restart is charged to the guilty VMs ([`VmStatus::charge_restart`], with
/// the commands each holds): one client's faults or hangs then cost only that client, however
/// many VMs it opens. A restart without a guilty VM, or one that keeps blaming a VM for more
/// than the work it held, is the device's.
fn classify<Q: Queues>(
    queues: &mut Q,
    entries: Entries,
    blamed: Option<u8>,
    attribution: Attribution,
    generation: u64,
) -> (u128, bool, bool, bool) {
    let mut guilty_qids = entries.guilty | blamed.map_or(0, |qid| 1u128 << qid);
    let cancelled = entries.cancelled;
    match attribution {
        Attribution::Firmware => {}
        Attribution::Queues(qids) | Attribution::Suspects(qids) => guilty_qids = qids,
        Attribution::NoCulprit => guilty_qids = 0,
    }
    let no_culprit = attribution == Attribution::NoCulprit;
    // Only the firmware, or the pass records' suspicion, names these queues. A reason-0 restart
    // can land on a bystander whose render started and waits behind the culprit's (2026-11-23
    // 01:46 UTC, m4.179: t3code, then Hyprland), and a slow innocent render can be the oldest
    // running one, so the first such blame is collateral and only a repeat makes the VM guilty.
    let unconfirmed = matches!(attribution, Attribution::Firmware | Attribution::Suspects(_));
    let blame = blamed.is_some_and(|qid| guilty_qids & (1u128 << qid) != 0);
    if guilty_qids == 0 && !no_culprit {
        return (0, blame, false, false);
    }
    let mut guilty: [Option<Arc<VmStatus>>; MAX_GUILTY_VMS] = Default::default();
    let mut count = 0;
    let mut collateral = 0u128;
    // Spared or unconfirmed work blamed a second time (`collateral_before`): its VM is guilty now.
    let mut repeated = 0u128;
    let mut unresolved = false;
    for qid in 0..QIDS {
        if guilty_qids & (1u128 << qid) == 0 {
            continue;
        }
        match queues.oldest_spared(qid) {
            None => unresolved = true,
            Some(spared) => {
                // Blame on a VM an earlier recovery found guilty needs no confirmation.
                let mut guilty_before = false;
                queues.visit_owners(qid, &mut |status| guilty_before |= status.recovery_guilty());
                // Without a mark a repeat looks like a first blame forever: such blame stays guilty.
                let suspect = spared || (unconfirmed && !guilty_before && queues.marks_collateral(qid));
                if suspect && !queues.collateral_before(qid, generation) {
                    collateral |= 1u128 << qid;
                    continue;
                }
                if suspect {
                    repeated |= 1u128 << qid;
                }
                let mut found = false;
                queues.visit_owners(qid, &mut |status| {
                    found = true;
                    if guilty[..count]
                        .iter()
                        .flatten()
                        .any(|known| Arc::ptr_eq(known, status))
                    {
                        return;
                    }
                    if let Some(slot) = guilty.get_mut(count) {
                        *slot = Some(status.clone());
                        count += 1;
                    } else {
                        unresolved = true;
                    }
                });
                unresolved |= !found;
            }
        }
    }
    if unresolved || (count == 0 && collateral == 0 && !no_culprit) {
        return (0, blame, false, false);
    }
    for qid in (0..QIDS).filter(|qid| collateral & (1u128 << qid) != 0) {
        queues.note_collateral(qid, generation);
    }
    let known = |status: &Arc<VmStatus>| guilty[..count].iter().flatten().any(|known| Arc::ptr_eq(known, status));
    let mut dropped = false;
    for qid in (0..QIDS).filter(|qid| entries.exact_qids & (1u128 << qid) != 0) {
        queues.visit_owners(qid, &mut |status| dropped |= known(status));
    }
    let leftover = count != 0 && dropped && guilty[..count].iter().flatten().all(|status| status.get() != 0);
    guilty[..count].iter().flatten().for_each(|status| status.note_recovery_guilty());
    // Its work failed already, at the collateral blame or as commands earlier restarts listed,
    // and failing it again changes nothing (`Pair::quarantine` skips failed work): nothing else
    // poisons this VM. Poison it here, after `leftover` judged this restart on the VM's state
    // before it, so that the restarts that only drop its work from now on are forgiven, and an
    // open or closed culprit is killed (2026-11-20 17:52 UTC, m4.175).
    for qid in (0..QIDS).filter(|qid| repeated & (1u128 << qid) != 0) {
        queues.visit_owners(qid, &mut |status| status.record(EIO));
    }
    // Commands each guilty VM holds, counted as they are classified.
    let held: [core::cell::Cell<u32>; MAX_GUILTY_VMS] = Default::default();
    let spared = queues.classify_work(&|status| {
        let known = guilty[..count].iter()
            .position(|known| known.as_ref().is_some_and(|known| Arc::ptr_eq(known, status)));
        match known {
            Some(index) => {
                held[index].set(held[index].get().saturating_add(1));
                Class::Guilty
            }
            // A VM an earlier recovery found guilty stays guilty. Spared, its leftover work would
            // make the next blame on it collateral, and that restart would count as a storm.
            None if status.recovery_guilty() => Class::Guilty,
            None => Class::Spared,
        }
    });
    // Every guilty VM is charged, even once one of them is over its count.
    let charged = count != 0 && guilty[..count].iter().zip(&held).fold(true, |charged, (status, held)| {
        status.as_ref().is_some_and(|status| status.charge_restart(held.get())) && charged
    });
    (cancelled & !(guilty_qids & !collateral) & spared, blame, leftover, charged)
}


#[kernel::macros::kunit_tests(asahi_g17_recovery_table_line)]
mod table_line_tests {
    use super::{Shown, SHOWN};
    use kernel::{prelude::*, str::CString};

    #[test]
    fn the_table_line_shows_queue_stamp_flags_and_match_then_a_count() {
        let mut shown = Shown::default();
        shown.push(1 | 2 | (41 << 4), 0x167a_1c00, b'E');
        shown.push(1 | (42 << 4), 0x7_9300, b'-');
        let line = CString::try_from_fmt(fmt!("{}", shown)).unwrap();
        assert_eq!(line.to_bytes(), b"q41:0x167a1c00:0x293/E q42:0x79300:0x2a1/-");
        for _ in 0..SHOWN {
            shown.push(1, 0, b'O');
        }
        let line = CString::try_from_fmt(fmt!("{}", shown)).unwrap();
        assert!(line.to_bytes().ends_with(b" +2"));
    }
}

#[kernel::macros::kunit_tests(asahi_g17_recovery_snapshot)]
mod tests {
    use super::*;
    use core::cell::Cell;

    struct Halted {
        recovery: State,
        state: Cell<u32>,
        reads: Cell<usize>,
        known_keys: bool,
        blamed: bool,
    }

    impl Halted {
        fn new(known_keys: bool) -> Self {
            let mut recovery = State::new();
            assert!(recovery.request(0, 38).unwrap());
            Self { recovery, state: Cell::new(1), reads: Cell::new(0),
                known_keys, blamed: false }
        }
    }

    impl Memory for Halted {
        fn is_m5(&self) -> bool { false }
        fn recovery_state(&self) -> Result<u32> { Ok(self.state.get()) }
        fn host_recovery(&self) -> Result<u32> { Ok(0) }
        fn fault_sources(&self) -> Result<Sources> {
            assert_eq!(self.state.get(), 1);
            let mut sources = Sources::sample(3, |slot| {
                Some((if self.known_keys { ((38 + slot as u64) << 40) | 1 } else { 0 }, 0))
            });
            sources.reason = Some(2);
            Ok(sources)
        }
        fn transition(&self, expected: u32, next: u32) -> Result {
            assert_eq!(self.state.get(), expected);
            self.state.set(next);
            Ok(())
        }
        fn clear_timestamps(&self) -> Result { panic!("active render scheduler") }
        fn entry(&self, _index: usize) -> Result<(u32, u32)> {
            assert_eq!(self.state.get(), 3);
            Ok((0, 0))
        }
        fn wait_tick(&self) {
            assert_eq!(self.state.get(), 2);
            self.state.set(3);
        }
    }

    impl Queues for Halted {
        fn render_scheduler_active(&self) -> bool { true }
        fn render_pass_started(&self, qid: u8, _stamp: u64) -> Option<bool> {
            // This is the regression oracle: state 0/2/3 is too late. A
            // resumed peer may have started after the fault we are attributing.
            assert_eq!(self.state.get(), 1);
            self.reads.set(self.reads.get() + 1);
            Some(qid == 39)
        }
        fn publish_recovery_targets(&self) -> Option<Error> { None }
        fn oldest_spared(&self, _qid: u8) -> Option<bool> { None }
        fn visit_owners(&self, _qid: u8, _visit: &mut dyn FnMut(&Arc<VmStatus>)) {}
        fn classify_work(&mut self, _class: &dyn Fn(&Arc<VmStatus>) -> Class) -> u128 { 0 }
        fn note_interrupted(&mut self, _qid: u8, _stamp: u32) -> Interrupted { Interrupted::None }
        fn settle_blamed_render(&mut self, qid: u8, _error: Error) {
            assert_eq!(qid, 38);
            self.blamed = true;
        }
        fn settle_cancelled_renders(&mut self, _qids: u128) {}
        fn rescan_compute(&mut self) -> Result { Ok(()) }
        fn release_retired_compute(&mut self) {}
    }

    impl Host for Halted {
        fn recovery(&self) -> &State { &self.recovery }
        fn recovery_mut(&mut self) -> &mut State { &mut self.recovery }
    }

    /// A halted recovery table holding `records` (index, flags); every other record is invalid.
    /// Records the QIDs whose commands the walk marks interrupted.
    struct Table {
        recovery: State,
        m5: bool,
        records: [(usize, u32); 5],
        marked: u128,
    }

    impl Table {
        fn new(m5: bool) -> Self {
            const VALID: u32 = 1;
            Self {
                recovery: State::new(),
                m5,
                records: [
                    (0, VALID | (12 << 4)),
                    (1, VALID | u32::from(GUILTY) | (17 << 4)),
                    // Index 2 is invalid: G17P's list ends here, and what follows is left over.
                    (3, VALID | u32::from(GUILTY | CANCELLED) | (40 << 4)),
                    (RECOVERY_RECORDS - 1, VALID | u32::from(GUILTY) | (41 << 4)),
                    (RECOVERY_RECORDS, VALID | u32::from(GUILTY) | (42 << 4)),
                ],
                marked: 0,
            }
        }
    }

    impl Memory for Table {
        fn is_m5(&self) -> bool { self.m5 }
        fn recovery_state(&self) -> Result<u32> { Ok(3) }
        fn host_recovery(&self) -> Result<u32> { Ok(0) }
        fn fault_sources(&self) -> Result<Sources> { Err(EIO) }
        fn transition(&self, _expected: u32, _next: u32) -> Result { Err(EIO) }
        fn clear_timestamps(&self) -> Result { Err(EIO) }
        fn entry(&self, index: usize) -> Result<(u32, u32)> {
            assert!(index < RECOVERY_RECORDS);
            let flags = self.records.iter().find(|(at, _)| *at == index).map_or(0, |(_, flags)| *flags);
            Ok((flags, index as u32))
        }
        fn wait_tick(&self) {}
    }

    impl Queues for Table {
        fn render_scheduler_active(&self) -> bool { true }
        fn render_pass_started(&self, _qid: u8, _stamp: u64) -> Option<bool> { None }
        fn publish_recovery_targets(&self) -> Option<Error> { None }
        fn oldest_spared(&self, _qid: u8) -> Option<bool> { None }
        fn visit_owners(&self, _qid: u8, _visit: &mut dyn FnMut(&Arc<VmStatus>)) {}
        fn classify_work(&mut self, _class: &dyn Fn(&Arc<VmStatus>) -> Class) -> u128 { 0 }
        fn note_interrupted(&mut self, qid: u8, _stamp: u32) -> Interrupted {
            self.marked |= 1u128 << qid;
            Interrupted::Exact
        }
        fn settle_blamed_render(&mut self, _qid: u8, _error: Error) {}
        fn settle_cancelled_renders(&mut self, _qids: u128) {}
        fn rescan_compute(&mut self) -> Result { Ok(()) }
        fn release_retired_compute(&mut self) {}
    }

    impl Host for Table {
        fn recovery(&self) -> &State { &self.recovery }
        fn recovery_mut(&mut self) -> &mut State { &mut self.recovery }
    }

    #[test]
    fn recovery_table_ends_at_its_first_invalid_record() {
        let mut host = Table::new(false);
        let entries = read_entries(&mut host);
        assert_eq!(entries.listed, 2);
        assert_eq!(entries.guilty, 1u128 << 17);
        assert_eq!(entries.cancelled, 0);
        assert_eq!(entries.exact, 2);
        assert_eq!(entries.exact_qids, (1u128 << 12) | (1u128 << 17));
        // Leftover records blame and cancel nothing and mark no bystander's command.
        assert_eq!(host.marked, (1u128 << 12) | (1u128 << 17));
    }

    #[test]
    fn m5_recovery_table_is_walked_past_invalid_records() {
        let mut host = Table::new(true);
        let entries = read_entries(&mut host);
        assert_eq!(entries.listed, 4);
        assert_eq!(entries.guilty, (1u128 << 17) | (1u128 << 40) | (1u128 << 41));
        assert_eq!(entries.cancelled, 1u128 << 40);
        // The M5 table only classifies.
        assert_eq!((entries.exact, entries.fallback, entries.exact_qids), (0, 0, 0));
        assert_eq!(host.marked, 0);
    }

    #[test]
    fn pass_evidence_is_frozen_before_the_first_acknowledgement() {
        let mut host = Halted::new(true);
        service(&mut host, || false).unwrap();
        assert_eq!(host.reads.get(), 2);
        assert_eq!(host.state.get(), 0);
        assert_eq!(host.recovery.generation(), 1);
        assert!(!host.blamed);
    }

    /// Two VMs, each owning one render pair: `a` queues 40/41, `b` queues 60/61.
    struct Owners([(u8, Arc<VmStatus>); 4], [Option<Class>; 4]);

    impl Queues for Owners {
        fn render_scheduler_active(&self) -> bool { true }
        fn render_pass_started(&self, _qid: u8, _stamp: u64) -> Option<bool> { None }
        fn publish_recovery_targets(&self) -> Option<Error> { None }
        fn oldest_spared(&self, qid: u8) -> Option<bool> {
            self.0.iter().any(|(own, _)| *own == qid).then_some(false)
        }
        fn visit_owners(&self, qid: u8, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
            self.0.iter().filter(|(own, _)| *own == qid).for_each(|(_, status)| visit(status));
        }
        fn classify_work(&mut self, class: &dyn Fn(&Arc<VmStatus>) -> Class) -> u128 {
            for (index, (_, status)) in self.0.iter().enumerate() {
                self.1[index] = Some(class(status));
            }
            0
        }
        fn note_interrupted(&mut self, _qid: u8, _stamp: u32) -> Interrupted { Interrupted::None }
        fn settle_blamed_render(&mut self, _qid: u8, _error: Error) {}
        fn settle_cancelled_renders(&mut self, _qids: u128) {}
        fn rescan_compute(&mut self) -> Result { Ok(()) }
        fn release_retired_compute(&mut self) {}
    }

    fn owners() -> Owners {
        let a = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();
        let b = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();
        Owners([(40, a.clone()), (41, a), (60, b.clone()), (61, b)], [None; 4])
    }

    fn leftover(queues: &mut Owners, exact_qid: u8) -> bool {
        let entries = Entries { exact_qids: 1u128 << exact_qid, ..Entries::default() };
        classify(queues, entries, Some(40), Attribution::Queues(3u128 << 60), 1).2
    }

    #[test]
    fn only_dropped_work_of_an_already_failed_vm_is_leftover() {
        let mut queues = owners();
        // A VM's first failure is not leftover work.
        assert!(!leftover(&mut queues, 60));
        queues.0[2].1.record(EIO);
        assert!(leftover(&mut queues, 60));
        // The restart dropped nothing of the failed VM.
        assert!(!leftover(&mut queues, 40));
    }

    #[test]
    fn restarts_are_charged_to_their_guilty_clients_and_only_unblamed_ones_to_the_device() {
        let mut queues = owners();
        let charged = |queues: &mut Owners, attribution| {
            classify(queues, Entries::default(), None, attribution, 1).3
        };
        // Nobody to blame: the device's.
        assert!(!charged(&mut queues, Attribution::NoCulprit));
        assert!(!charged(&mut queues, Attribution::Firmware));
        // Each VM pays for its first restart, then for one more per command it held then (two
        // each here).
        assert!(charged(&mut queues, Attribution::Queues(1u128 << 60)));
        for _ in 0..3 {
            assert!(charged(&mut queues, Attribution::Queues(1u128 << 40)));
        }
        // A firmware that keeps blaming a for work it no longer holds stops the GPU as before,
        // even with b, still within its count, blamed beside it.
        assert!(!charged(&mut queues, Attribution::Queues(1u128 << 40)));
        assert!(!charged(&mut queues, Attribution::Queues((1u128 << 40) | (1u128 << 60))));
        assert!(charged(&mut queues, Attribution::Queues(1u128 << 60)));
        assert!(!charged(&mut queues, Attribution::Queues(1u128 << 60)));
    }

    #[test]
    fn unconfirmed_blame_without_a_collateral_mark_is_guilty_at_once() {
        // A compute queue, or any queue on M5, keeps no mark: a repeat could never be seen.
        let mut queues = owners();
        let entries = Entries { exact_qids: 1u128 << 40, ..Entries::default() };
        classify(&mut queues, entries, Some(40), Attribution::Firmware, 1);
        assert!(queues.0[0].1.recovery_guilty());
        assert_eq!(queues.1, [Some(Class::Guilty), Some(Class::Guilty), Some(Class::Spared), Some(Class::Spared)]);
    }

    #[test]
    fn a_guilty_vm_is_not_spared_by_another_vms_recovery() {
        let mut queues = owners();
        let entries = || Entries { exact_qids: 1u128 << 40, ..Entries::default() };
        // b failing for its own client does not make it guilty in a's recovery.
        queues.0[2].1.record(EIO);
        classify(&mut queues, entries(), Some(40), Attribution::Queues(3u128 << 40), 1);
        assert_eq!(queues.1, [Some(Class::Guilty), Some(Class::Guilty), Some(Class::Spared), Some(Class::Spared)]);
        // Once a recovery finds b guilty, a later recovery that blames a keeps b guilty.
        classify(&mut queues, entries(), Some(60), Attribution::Queues(3u128 << 60), 1);
        classify(&mut queues, entries(), Some(40), Attribution::Queues(3u128 << 40), 1);
        assert_eq!(queues.1, [Some(Class::Guilty); 4]);
    }

    /// A render pair in a cascade of restarts (see [`Cascade`]).
    struct CascadePair {
        qids: [u8; 2],
        status: Arc<VmStatus>,
        class: Option<Class>,
        collateral: Option<u64>,
        /// Its work failed already (a restart listed it, or it was blamed): failing it again
        /// changes nothing, as `Pair::quarantine` skips failed work.
        failed: bool,
    }

    /// The render pairs across a cascade of restarts, with state: each pair keeps the class the
    /// last recovery gave its work (`Pair::classify`) and its collateral mark
    /// (`Pair::note_collateral`), which a retirement of its work clears. A blamed render fails as
    /// `settle_blamed_render` fails it: guilty work poisons its VM, work an earlier recovery
    /// spared fails without poisoning it (`Completion::poisons`), and work that failed already
    /// is not failed again.
    struct Cascade([Option<CascadePair>; 4], u64);

    impl Cascade {
        fn new(pairs: [([u8; 2], &Arc<VmStatus>); 4]) -> Self {
            Self(
                pairs.map(|(qids, status)| {
                    Some(CascadePair { qids, status: status.clone(), class: None, collateral: None, failed: false })
                }),
                0,
            )
        }
        fn pair(&self, qid: u8) -> Option<&CascadePair> {
            self.0.iter().flatten().find(|pair| pair.qids.contains(&qid))
        }
        fn pair_mut(&mut self, qid: u8) -> Option<&mut CascadePair> {
            self.0.iter_mut().flatten().find(|pair| pair.qids.contains(&qid))
        }
        /// The pair that holds `qid` is bound to `status`'s owner in place, as `Pair::bind_owner`
        /// rebinds a reusable pair: its mark goes as there (`rebound_collateral`), its work is new.
        fn bind(&mut self, qid: u8, status: &Arc<VmStatus>) {
            if let Some(pair) = self.pair_mut(qid) {
                let same = Arc::ptr_eq(&pair.status, status);
                pair.collateral = crate::g17::queue::render::rebound_collateral(pair.collateral, same);
                pair.status = status.clone();
                pair.class = None;
                pair.failed = false;
            }
        }
        /// The pair that holds `qid` submitted new work that no recovery has classified yet, and
        /// it is now the pair's oldest in flight (its earlier work failed or retired).
        fn submit(&mut self, qid: u8) {
            if let Some(pair) = self.pair_mut(qid) {
                pair.class = None;
                pair.failed = false;
            }
        }
        /// Work of the pair that holds `qid` retired (`Pair::retire_front_command`).
        fn retire(&mut self, qid: u8) {
            if let Some(pair) = self.pair_mut(qid) {
                pair.collateral = None;
                pair.failed = false;
            }
        }
        /// One restart as `service` runs it: the firmware blames `blamed`, its table drops
        /// `dropped` by exact stamp, the blamed render fails. Returns whether it was forgiven.
        fn restart(&mut self, blamed: u8, dropped: u128) -> bool {
            self.restart_as(blamed, dropped, Attribution::Firmware)
        }
        /// As [`Self::restart`], with render attribution (`render_culprit`) deciding the blame.
        fn restart_as(&mut self, blamed: u8, dropped: u128, attribution: Attribution) -> bool {
            let entries = Entries { exact_qids: dropped, ..Entries::default() };
            self.1 += 1;
            let generation = self.1;
            let (_, blame, leftover, _) = classify(self, entries, Some(blamed), attribution, generation);
            // The listed commands fail: a guilty VM's poison it, others' are discarded.
            for pair in self.0.iter_mut().flatten() {
                if !pair.failed && pair.qids.iter().any(|qid| dropped & (1u128 << qid) != 0) {
                    if pair.class == Some(Class::Guilty) {
                        pair.status.record(EIO);
                    }
                    pair.failed = true;
                }
            }
            if blame {
                self.settle_blamed_render(blamed, EIO);
            }
            leftover
        }
    }

    impl Queues for Cascade {
        fn render_scheduler_active(&self) -> bool { true }
        fn render_pass_started(&self, _qid: u8, _stamp: u64) -> Option<bool> { None }
        fn publish_recovery_targets(&self) -> Option<Error> { None }
        fn oldest_spared(&self, qid: u8) -> Option<bool> {
            self.pair(qid).map(|pair| pair.class == Some(Class::Spared))
        }
        fn collateral_before(&self, qid: u8, generation: u64) -> bool {
            self.pair(qid).is_some_and(|pair| pair.collateral.is_some_and(|at| at < generation))
        }
        fn note_collateral(&mut self, qid: u8, generation: u64) {
            if let Some(pair) = self.pair_mut(qid) {
                pair.collateral.get_or_insert(generation);
            }
        }
        fn marks_collateral(&self, qid: u8) -> bool { self.pair(qid).is_some() }
        fn visit_owners(&self, qid: u8, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
            if let Some(pair) = self.pair(qid) {
                visit(&pair.status);
            }
        }
        fn classify_work(&mut self, class: &dyn Fn(&Arc<VmStatus>) -> Class) -> u128 {
            let mut spared = 0;
            for pair in self.0.iter_mut().flatten() {
                pair.class = Some(class(&pair.status));
                if pair.class == Some(Class::Spared) {
                    spared |= (1u128 << pair.qids[0]) | (1u128 << pair.qids[1]);
                }
            }
            spared
        }
        fn note_interrupted(&mut self, _qid: u8, _stamp: u32) -> Interrupted { Interrupted::Exact }
        fn settle_blamed_render(&mut self, qid: u8, error: Error) {
            let Some(pair) = self.pair_mut(qid) else { return };
            if pair.failed {
                return;
            }
            pair.failed = true;
            if pair.class == Some(Class::Spared) {
                pair.status.report_failure(error);
            } else {
                pair.class = Some(Class::Guilty);
                pair.status.record(error);
            }
        }
        fn settle_cancelled_renders(&mut self, _qids: u128) {}
        fn rescan_compute(&mut self) -> Result { Ok(()) }
        fn release_retired_compute(&mut self) {}
    }

    fn vm() -> Arc<VmStatus> {
        Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap()
    }

    #[test]
    fn a_culprit_spared_by_a_misattributed_first_restart_turns_guilty() {
        // 11-20 15:03 UTC (m4.174), the 11-19 19:12 storm's shape: c spins on 40/41, and the
        // first restart blames b (44/45), whose work c starved. Nothing confirms the blame: b's
        // command fails without poisoning its VM; c's work is spared.
        let (c, b, x) = (vm(), vm(), vm());
        let mut queues = Cascade::new([([40, 41], &c), ([44, 45], &b), ([60, 61], &x), ([62, 63], &x)]);
        assert!(!queues.restart(45, (1 << 41) | (1 << 45)));
        assert!(!b.recovery_guilty() && b.get() == 0);
        // The next restart blames b again with nothing of b retired: the repeat makes b guilty.
        // Blame alone cannot tell a bystander starved twice from a culprit.
        assert!(!queues.restart(45, 1 << 45));
        assert!(b.recovery_guilty() && b.get() != 0);
        assert!(queues.restart(45, 1 << 45));
        // Then every restart blames c's queue 41 and drops its running group by exact stamp.
        // The first blame on work an earlier restart spared is collateral: c fails without
        // poisoning its VM, and the restart counts.
        assert!(!queues.restart(41, 1 << 41));
        assert!(!c.recovery_guilty() && c.get() == 0);
        // Blamed again with nothing of it retired since: the hang is c's own.
        assert!(!queues.restart(41, 1 << 41));
        assert!(c.recovery_guilty());
        assert_ne!(c.get(), 0);
        // From now on a restart that only drops failed c's work is forgiven.
        assert!(queues.restart(41, 1 << 41));
    }

    #[test]
    fn a_bystander_that_retires_between_blames_stays_collateral() {
        // c is blamed first, twice; b's work is spared, then blamed while it waits behind c's.
        let (c, b, x) = (vm(), vm(), vm());
        let mut queues = Cascade::new([([40, 41], &c), ([44, 45], &b), ([60, 61], &x), ([62, 63], &x)]);
        queues.restart(41, 1 << 41);
        queues.restart(41, 1 << 41);
        assert!(c.recovery_guilty());
        queues.restart(45, 1 << 45);
        assert!(!b.recovery_guilty() && b.get() == 0);
        // b's next command retires: blamed again, it is collateral again, its VM unpoisoned.
        queues.retire(44);
        queues.restart(45, 1 << 45);
        assert!(!b.recovery_guilty() && b.get() == 0);
        // Blamed a third time with nothing retired since: guilty. Its client already saw a
        // lost device at each blame; now its VM is poisoned too.
        queues.restart(45, 1 << 45);
        assert!(b.recovery_guilty());
        assert_ne!(b.get(), 0);
    }

    #[test]
    fn a_retirement_on_another_queue_of_the_culprit_does_not_reset_it() {
        // c owns two pairs, 40/41 (spinning) and 50/51; the first restart blames b.
        let (c, b, x) = (vm(), vm(), vm());
        let mut queues = Cascade::new([([40, 41], &c), ([50, 51], &c), ([44, 45], &b), ([60, 61], &x)]);
        queues.restart(45, (1 << 41) | (1 << 45));
        queues.restart(41, 1 << 41);
        assert!(!c.recovery_guilty());
        // c's other pair keeps retiring: the blamed pair's mark stays.
        queues.retire(50);
        queues.restart(41, 1 << 41);
        assert!(c.recovery_guilty());
        assert_ne!(c.get(), 0);
    }

    #[test]
    fn a_new_owner_of_a_reused_queue_starts_with_a_fresh_collateral() {
        let (c, b, d, x) = (vm(), vm(), vm(), vm());
        let mut queues = Cascade::new([([40, 41], &c), ([44, 45], &b), ([60, 61], &x), ([62, 63], &x)]);
        queues.restart(45, (1 << 41) | (1 << 45));
        queues.restart(41, 1 << 41);
        assert!(!c.recovery_guilty());
        // c closes without retiring anything; its pair, reused in place, is bound to d.
        queues.bind(41, &d);
        // A restart that blames b again spares d's work; then one blames d's queue 41.
        queues.restart(45, 1 << 45);
        queues.restart(41, 1 << 41);
        assert!(!d.recovery_guilty() && d.get() == 0);
        // A rebind for the same owner (a pool change) keeps the mark: blamed again, d is guilty.
        queues.bind(41, &d);
        queues.restart(45, 1 << 45);
        queues.restart(41, 1 << 41);
        assert!(d.recovery_guilty());
    }

    #[test]
    fn the_11_19_storm_ends_with_its_culprit_guilty_before_the_limit() {
        // 2026-11-19 19:12 UTC (m4.171): deqp-vk (40/41) spins. Gen 0 blamed chromium (62/63),
        // gen 1 deqp, gen 2 quickshell (42/43), gens 3-7 deqp again; every one counted until
        // the storm limit stopped the GPU (8 in 30 s).
        let (deqp, chromium, quickshell, x) = (vm(), vm(), vm(), vm());
        let mut queues =
            Cascade::new([([40, 41], &deqp), ([62, 63], &chromium), ([42, 43], &quickshell), ([60, 61], &x)]);
        let mut counted = 0;
        for (blamed, dropped) in [(63, 1u128 << 63), (41, 1 << 41), (42, 1 << 42)] {
            counted += usize::from(!queues.restart(blamed, dropped));
        }
        // chromium's and quickshell's first blames were collateral: each saw one failed command,
        // and their VMs stay usable.
        assert!(!chromium.recovery_guilty() && chromium.get() == 0);
        assert!(!quickshell.recovery_guilty() && quickshell.get() == 0);
        // deqp's second blame makes it guilty, and later restarts that drop its work are forgiven.
        for _ in 3..8 {
            counted += usize::from(!queues.restart(41, 1 << 41));
        }
        assert!(deqp.recovery_guilty() && deqp.get() != 0);
        assert_eq!(counted, 4);
    }

    #[test]
    fn the_m4_175_cascade_fails_its_culprit_whose_work_already_failed() {
        // 2026-11-20 17:52 UTC (m4.175): deqp-vk (61/62) spins and its commands fail as listed
        // ones from the first restart on. Chromium's GPU process (58/59), then its next process
        // on the same pair, then a third (63/64) are blamed first.
        let (chromium, chromium2, chromium3, deqp) = (vm(), vm(), vm(), vm());
        let mut queues =
            Cascade::new([([58, 59], &chromium), ([61, 62], &deqp), ([63, 64], &chromium3), ([60, 65], &chromium2)]);
        let mut counted = 0;
        counted += usize::from(!queues.restart(59, (1 << 59) | (1 << 62)));
        queues.bind(58, &chromium2);
        counted += usize::from(!queues.restart(58, 1 << 58));
        // The firmware blames deqp's 62; attribution moves it to chromium's failed pair.
        counted += usize::from(!queues.restart_as(62, 3 << 58, Attribution::Queues(3 << 58)));
        counted += usize::from(!queues.restart(64, (1 << 62) | (1 << 64)));
        // deqp's 62, blamed once (collateral), then again: guilty and failed, although every
        // command of it failed already as a listed one.
        counted += usize::from(!queues.restart(62, 1 << 62));
        assert!(!deqp.recovery_guilty() && deqp.get() == 0);
        counted += usize::from(!queues.restart(62, 1 << 62));
        assert!(deqp.recovery_guilty());
        assert_ne!(deqp.get(), 0);
        // The next restarts drop only failed deqp's work: forgiven, under the storm limit. The
        // chromium processes' first blames are collateral, so the restart that attribution moved
        // to chromium2's pair (its second) counts too.
        assert!(queues.restart_as(62, 1 << 62, Attribution::Queues(3 << 61)));
        assert!(queues.restart(62, 1 << 62));
        assert!(!chromium.recovery_guilty() && chromium.get() == 0);
        assert!(!chromium3.recovery_guilty() && chromium3.get() == 0);
        assert_eq!(counted, 6);
    }

    #[test]
    fn a_culprit_blamed_first_is_guilty_at_its_second_blame() {
        // 2026-11-20 02:18 UTC (m4.173, T72 run 2): the first restart blamed deqp's 59. Nothing
        // confirmed that blame, so it is collateral: one failed command, the VM stays usable.
        let (deqp, b, x) = (vm(), vm(), vm());
        let mut queues = Cascade::new([([58, 59], &deqp), ([44, 45], &b), ([60, 61], &x), ([62, 63], &x)]);
        assert!(!queues.restart(59, 1 << 45));
        assert!(!deqp.recovery_guilty() && deqp.get() == 0);
        // Its hang comes back with nothing of it retired: the blame is its own.
        assert!(!queues.restart(59, 1 << 59));
        assert!(deqp.recovery_guilty() && deqp.get() != 0);
        assert!(queues.restart(59, 1 << 59));
        assert!(!b.recovery_guilty() && b.get() == 0);
    }

    #[test]
    fn the_oldest_running_render_is_only_suspected_at_first() {
        // An innocent slow render that started its tiler just before the hung one is the oldest
        // running render. The pass records only suspect it: collateral once, its VM kept.
        let (slow, deqp, x) = (vm(), vm(), vm());
        let mut queues = Cascade::new([([58, 59], &slow), ([60, 61], &deqp), ([62, 63], &x), ([64, 65], &x)]);
        assert!(!queues.restart_as(59, 1 << 59, Attribution::Suspects(3 << 58)));
        assert!(!slow.recovery_guilty() && slow.get() == 0);
        // It retires; deqp's render is the oldest running one now and is suspected twice.
        queues.retire(58);
        queues.restart_as(59, 1 << 61, Attribution::Suspects(3 << 60));
        assert!(!deqp.recovery_guilty());
        queues.restart_as(59, 1 << 61, Attribution::Suspects(3 << 60));
        assert!(deqp.recovery_guilty() && deqp.get() != 0);
        assert!(!slow.recovery_guilty() && slow.get() == 0);
    }

    #[test]
    fn a_confirmed_first_blame_is_guilty_at_once() {
        // Evidence (slot identity, the host's pass records or a latched MMU fault) names the VM.
        let (deqp, b, x) = (vm(), vm(), vm());
        let mut queues = Cascade::new([([58, 59], &deqp), ([44, 45], &b), ([60, 61], &x), ([62, 63], &x)]);
        assert!(!queues.restart_as(59, 1 << 59, Attribution::Queues(3 << 58)));
        assert!(deqp.recovery_guilty() && deqp.get() != 0);
        assert!(queues.restart(59, 1 << 59));
        assert!(!b.recovery_guilty() && b.get() == 0);
    }

    #[test]
    fn the_m4_179_bystander_blames_poison_only_the_culprit() {
        // 2026-11-23 01:46:44 UTC (m4.179, #2504 hang row run 7): deqp-vk (100/101) spins. Reason
        // 0, no slot keys, and each bystander's front render had its tiler start written, so no
        // evidence moved the blame. Gen 21 blamed t3code's 57, gen 22 Hyprland's 37, gen 23
        // deqp's 101; both bystanders were failed as guilty and Hyprland lost the session.
        let (deqp, t3code, hyprland, x) = (vm(), vm(), vm(), vm());
        let mut queues =
            Cascade::new([([100, 101], &deqp), ([56, 57], &t3code), ([36, 37], &hyprland), ([60, 61], &x)]);
        let mut counted = 0;
        counted += usize::from(!queues.restart(57, (3 << 56) | (1 << 101)));
        assert!(!t3code.recovery_guilty() && t3code.get() == 0);
        // Hyprland's next frame started after that restart: nothing spared it.
        queues.submit(36);
        counted += usize::from(!queues.restart(37, (3 << 36) | (1 << 101)));
        assert!(!hyprland.recovery_guilty() && hyprland.get() == 0);
        counted += usize::from(!queues.restart(101, 3 << 100));
        assert!(!deqp.recovery_guilty());
        counted += usize::from(!queues.restart(101, 1 << 101));
        assert!(deqp.recovery_guilty() && deqp.get() != 0);
        assert!(queues.restart(101, 1 << 101));
        assert!(!t3code.recovery_guilty() && t3code.get() == 0);
        assert!(!hyprland.recovery_guilty() && hyprland.get() == 0);
        assert_eq!(counted, 4);
    }

    #[test]
    fn unknown_25g83_keys_still_preserve_firmware_blame() {
        let mut host = Halted::new(false);
        service(&mut host, || false).unwrap();
        assert_eq!(host.reads.get(), 0);
        assert!(host.blamed);
        assert_eq!(host.state.get(), 0);
    }
}
