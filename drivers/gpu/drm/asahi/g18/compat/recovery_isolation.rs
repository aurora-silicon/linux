// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Recovery isolation (`g17p_recovery_isolation`): after a firmware
//! stamp-blamed recovery only the guilty client loses.
//!
//! Root-only sysfs, runtime-writable bitmask; also settable at load. Writing
//! 0 restores the accepted Y behaviour exactly (no completion is ever marked,
//! no admission wait is taken). Writing "reset" clears the counters.
//!
//! Hardware evidence (saved kernel logs, 2026-09-30/10-01): a stamp-blamed
//! recovery cancels in-flight work of EVERY client, and Y then poisons each
//! affected VM's sticky status, which Mesa turns into VK_ERROR_DEVICE_LOST:
//!   * mmuf2 01:10:21 -- blamed qid 10 (Hyprland, VM 15). Xwayland's compute
//!     qid 7 (VM 17) retired zero-duration -> `fail_while_owned(ENODATA)`
//!     -> VM 17 status -61 -> every map refused -> Xwayland died.
//!   * hypr-crash-0224 02:24:31 -- blamed qid 5 (another client). Hyprland's
//!     parked 3D (qid 14) never ran after the recovery; its 2 s timeout
//!     retired it unexecuted -> `complete(Err(EIO))` -> VM 43 status -5 ->
//!     Hyprland aborted.
//!
//! Recovery-info entry layout. Native decode (static RE of the J700 kext,
//! `AGXArmFirmware::processFirmwareInitiatedRecovery` in
//! neo-stability-20260930/native/kext25.dis @fffffe00088a8398): it walks all
//! 256 8-byte entries, skips those without bit0, takes qid = bits 4..10
//! (`ubfx w24, w22, #4, #7`), looks up the command by (qid, word1) and stores
//! a per-command error code: 5 when bit1 is clear, otherwise 3 (or 0xb when
//! the restart reason is 3). In public macOS logs 5 is
//! kIOGPUCommandBufferCallbackErrorInnocentVictim, 3 Hang, 0xb PageFault
//! (names from macOS log strings, NOT from this binary). So bit1 = GUILTY,
//! and native fails every listed non-guilty command with InnocentVictim --
//! per command buffer, no replay seen in this function. Linux captures agree:
//! bit1 is set exactly on the queue the host's blamed stamp names in 4/4
//! recoveries. bit2 is set on queues the firmware cancelled, bit3 on TA
//! queues (captures; native only forwards bit2 into its restart report).
//!
//!   bit0 INNOCENT: classify every in-flight completion at each recovery.
//!        A VM owns none of the guilty queues (blamed stamp + entries with
//!        bit1) -> its cancelled work fails with an error fence ONLY: no
//!        sticky VM status, no VM-wide fan-out. Its compute queue, once the
//!        existing firmware-retirement witness proves the cancelled kicks
//!        retired, returns to service instead of staying quarantined for the
//!        boot. Unresolvable blame (a guilty qid whose owner cannot be found)
//!        disables isolation for that recovery: today's behaviour.
//!   bit1 ADMISSION_WAIT: a VM whose status is clean but whose mappings are
//!        transiently blocked by retained (quarantined) work waits up to
//!        ADMISSION_WAIT_MS for the release instead of refusing VM_BIND, and
//!        a compute submit waits up to SUBMIT_WAIT_MS for the retirement
//!        witness of its own spared queue. Poisoned VMs still refuse at once.
//!        (iso3) A render submit whose owner has every pair quarantined
//!        waits up to ADMISSION_WAIT_MS for the reclaim that returns the
//!        pair, instead of Y's immediate EIO, which poisons the VM through
//!        `fail_unpublished`. Field case: boot d324fcdd 05:38:44, spared VM
//!        2559's pair (slot 11) was quarantined at .370767, its next render
//!        failed at .371211 and poisoned the VM, and the reclaim returned the
//!        pair at .373928. An owner with a healthy pair never waits here.
//!   bit2 SETTLE_CANCELLED: spared renders the firmware listed as CANCELLED
//!        (bit2, not bit1) are settled at the end of the recovery through the
//!        same exact-once settle the blamed slot uses, instead of waiting for
//!        the 2 s scheduler watchdog. A group firmware has not provably
//!        retired keeps its pair quarantined (mappings retained) until the
//!        reclaim witness. Listed-only entries (bit2 clear) are left alone.
//!   bit3 GUILTY_GRACE: the guilty VM is also spared, at most once per
//!        GRACE_WINDOW_NS per VM; a second guilty recovery inside the window
//!        poisons it as today. Opt-in: this drops the guilty submission
//!        silently instead of reporting device loss.
//!   bit4 RESUME_DOORBELL (opt-in, A/B): when spared render work the firmware
//!        did not cancel is still pending after the recovery, send ONE paired
//!        render doorbell (the message every render submission ends with;
//!        it names no queue) so firmware rescans its rings. 02:24 evidence:
//!        Hyprland's parked 3D (entry flags 0x1, not cancelled) never ran in
//!        the 2 s after the recovery. Whether a doorbell activates it is the
//!        question this bit answers; it is not established.
//!
//!   bit6 REPLAY (iso4, default on): Vulkan has no per-command-buffer error
//!        (native Metal reports InnocentVictim per command buffer; a Vulkan
//!        client can only be told "device lost" or be handed wrong results),
//!        so a spared command that provably never started is published again
//!        instead of failing. Compute: a spared zero-duration queue's kicks
//!        are kept across the retirement witness; only if EVERY kept kick's
//!        own completion words still show start == end (firmware retired it
//!        without executing, or never reached it) after the witness, and the
//!        queue was returned to service, are they published again, oldest
//!        first, before any newer submission of that queue (the compute
//!        backend waits up to SUBMIT_WAIT_MS for the witness). Fences stay
//!        unsignalled and CLASS_SPARED throughout, so nothing on this path
//!        can poison the VM; a refused or failed replay fails that kick and
//!        all its successors exactly as before (ENODATA, spared). A command
//!        is replayed at most MAX_REPLAYS times, and each replay grants its
//!        scheduler job one fresh timeout period (the recovery consumed the
//!        old one). A replayed front kick is never treated as an earlier
//!        recovery's collateral: if a later recovery blames its QID, it ran on
//!        its own after the replay and its owner is guilty (no replay loop).
//!        A kick that a render already named as its native parent is not
//!        replayed (see `ComputeReplayQueue::native_consumer`).
//!        Renders are NOT replayed yet; spared renders failed after
//!        firmware retirement are counted by whether their pass started
//!        (`spared-render-started/-unstarted`) to size that work.
//!        Evidence level: "start == end means never executed" is the driver's
//!        existing zero-duration interpretation (`g17p_zero_duration`), and
//!        kicks of one QID run in publication order, so a zero-duration front
//!        kick implies none behind it ran. It is not independently decoded
//!        from native firmware. Every in-flight kick snapshotted at recovery
//!        in the saved logs (199 snapshots) had page words [0, 0].
//!   bit0 also covers inherited errors (iso4): a consumer whose dependency
//!        failed fails with ECANCELED instead of the dependency's EIO /
//!        ETIMEDOUT / ENODEV, so it is not charged as its own VM's device
//!        loss. Since c209c3dc5, consumers of a producer with host-copied
//!        timestamps wait for its completion fence, so they always see a
//!        failed producer's error; a spared render retired by firmware
//!        completes with EIO, which used to poison every consumer's VM.
//!
//! v2 (iso2): a recovery that blames a queue whose OLDEST in-flight work was
//! already spared by an earlier recovery treats it as that recovery's
//! collateral and spares its owner again (counter `collateral`); and the
//! first job guard of a submission takes exactly Y's path unless Y's path
//! refuses it (no extra locking on the hot path).
//!
//! v3 (iso3) bit5 ATTRIBUTION (default on): the firmware's blame is the
//! first set bit of the hardware-slot fault masks (B1 `FUN_5450`, see
//! `PRIMARY_STATUS_B_RECOVERY_SLOT_MASK_OFFSET`), scanned TA first (inferred
//! from captures), not the slot that stalled.
//! With two or more sources, if the blamed slot's pass provably never
//! started (its tiler never stamped the pass start, and no DM1 progress
//! record exists for it), the culprit is re-attributed to the other sources
//! whose pass provably started (stamped pass start). If none did, no client
//! is guilty. A blamed source
//! with any execution evidence, a single-source fault and every page fault
//! (reason 3) keep the firmware's blame. Field case: 06:02:19 boot d324fcdd,
//! mask 0x11: slot 0 = quickshell TA qid 9 (pass start 0, no record) was
//! blamed; slot 4 = Minecraft 3D qid 27 (pass started, DM1 record state 2)
//! had stalled.
//!
//! bit7 VICTIM_DEFER (t-1179, default on): a HOST scheduler timeout is
//!        not a firmware recovery, so nothing above classifies it. Field case
//!        (boot of 2026-10-06 06:47:56, census J700-003): a vkd3d-proton test
//!        (owner 18911, qid 72, vm 25834) stopped retiring compute; its 2 s
//!        watchdog quarantined it. gnome-shell's compute kick (owner 3, qid 4,
//!        vm 5) had been published right behind it (submissions 3131223 and
//!        3131225) and never ran: global GPU progress moved by one in those
//!        6 s. Its own watchdog fired at 06:48:02, poisoned VM 5 with -110 and
//!        failed its render slots; firmware retired BOTH queues at 06:48:05,
//!        three seconds later, but VM 5 refused every later map and the
//!        compositor aborted. So a compute timeout whose queue's oldest kick
//!        was published after an unfinished kick of ANOTHER VM's queue is
//!        treated as head-of-line blocked: its scheduler timeout is renewed,
//!        at most VICTIM_MAX_RENEWALS times, and nothing else changes (a
//!        firmware recovery meanwhile classifies its kicks as usual). If the
//!        blocker is still unretired when the budget runs out, its in-flight
//!        kicks are classified CLASS_SPARED before the quarantine: their
//!        fences fail, the VM status is untouched and the queue returns after
//!        the retirement witness. Without a blocker, or once the blocker
//!        retired, a timeout is the queue's own and keeps today's behaviour
//!        (VM poisoned). A queue quarantined without retirement tickets
//!        never counts as a blocker: its kicks stay listed until processor
//!        stop, whatever firmware did. Kicks of different QIDs share the
//!        compute pipe in publication order -- the field case shows that
//!        blocking on G17P and the same client pair shows it on G13 (a 3 s
//!        kick delayed another process's tiny kick by 2.998 s), but it is not
//!        decoded from firmware -- so the renewal is
//!        bounded, and the oldest unretired kick never has a blocker: a stall
//!        always charges at least that queue.
//!
//! Memory safety is unchanged in every mode: packets, VM-job guards and
//! quarantined pairs are retained and released only by the existing
//! witnesses (exact retirement, firmware drain/reclaim, processor stop).
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
#[cfg(not(test))]
use kernel::{bindings, module_param::KernelParam};

pub(crate) const INNOCENT: u32 = 1 << 0;
pub(crate) const ADMISSION_WAIT: u32 = 1 << 1;
pub(crate) const SETTLE_CANCELLED: u32 = 1 << 2;
pub(crate) const GUILTY_GRACE: u32 = 1 << 3;
pub(crate) const RESUME_DOORBELL: u32 = 1 << 4;
pub(crate) const ATTRIBUTION: u32 = 1 << 5;
pub(crate) const REPLAY: u32 = 1 << 6;
pub(crate) const VICTIM_DEFER: u32 = 1 << 7;
const ALL_BITS: u32 = INNOCENT | ADMISSION_WAIT | SETTLE_CANCELLED | GUILTY_GRACE | RESUME_DOORBELL
    | ATTRIBUTION | REPLAY | VICTIM_DEFER;
/// Correctness/containment fixes with no hot-path cost are on; the guilty
/// grace changes the guilty client's semantics and stays opt-in.
pub(crate) const DEFAULT_MASK: u32 = INNOCENT | ADMISSION_WAIT | SETTLE_CANCELLED | ATTRIBUTION | REPLAY
    | VICTIM_DEFER;

pub(crate) const ADMISSION_WAIT_MS: u32 = 1000;
/// iso4 REPLAY: publications of one command beyond its first.
pub(crate) const MAX_REPLAYS: u8 = 3;
pub(crate) const SUBMIT_WAIT_MS: u32 = 200;
pub(crate) const GRACE_WINDOW_NS: u64 = 60_000_000_000;
/// bit7: scheduler periods (2 s each) a head-of-line blocked compute job may
/// wait for its blocker beyond its own. The field blocker retired 9 s after
/// its own timeout; 8 extra periods cover that with margin.
pub(crate) const VICTIM_MAX_RENEWALS: u8 = 8;

static MASK: AtomicU32 = AtomicU32::new(DEFAULT_MASK);

#[inline(always)]
pub(crate) fn enabled(bit: u32) -> bool { MASK.load(Ordering::Relaxed) & bit != 0 }

#[inline(always)]
pub(crate) fn mask() -> u32 { MASK.load(Ordering::Relaxed) }

// ---- REPLAY decision (bit6) ---------------------------------------------------

/// Host state of one quarantined compute queue at its retirement witness.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct ComputeReplayQueue {
    /// Every lost kick was spared and its fence is still unsignalled.
    pub(crate) spared_deferred: bool,
    /// The queue will be returned to service by this witness.
    pub(crate) spared_quarantine: bool,
    /// The owner has released (closed) the queue.
    pub(crate) released: bool,
    /// The queue still belongs to the witness ticket's owner.
    pub(crate) owner_matches: bool,
    /// A render named one of these kicks as its native parent. Firmware
    /// retired the cancelled kick, which satisfies that barrier, so the
    /// render may already have run without the kick's output; a replay would
    /// then write after its consumer read.
    pub(crate) native_consumer: bool,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum ComputeReplay {
    /// Not a spared deferred queue, or the bits are off: unchanged path.
    NotCandidate,
    /// A candidate whose kicks cannot all be proven unexecuted: they fail
    /// exactly as before (ENODATA, spared).
    Refused,
    /// Every kick provably never executed: keep them and publish again,
    /// oldest first, once the queue is back in service.
    Replay,
}

/// A kick's own completion words, read after the retirement witness, prove
/// it never executed when start == end: firmware's zero-duration retirement
/// shape, or both words still at the zeroed allocation value.
pub(crate) const fn never_executed(words: [u64; 2]) -> bool { words[0] == words[1] }

/// `kicks` yields, oldest first, each kick's completion words, or None when
/// its fence is already signalled or the words are unreadable.
pub(crate) fn compute_replay_decision<I>(mask: u32, queue: ComputeReplayQueue, kicks: I) -> ComputeReplay
where I: IntoIterator<Item = Option<[u64; 2]>> {
    if mask & (REPLAY | INNOCENT) != (REPLAY | INNOCENT)
        || !queue.spared_deferred || !queue.spared_quarantine
        || queue.released || !queue.owner_matches
    {
        return ComputeReplay::NotCandidate;
    }
    if queue.native_consumer {
        return ComputeReplay::Refused;
    }
    let mut any = false;
    for words in kicks {
        any = true;
        match words {
            Some(words) if never_executed(words) => {}
            _ => return ComputeReplay::Refused,
        }
    }
    if any { ComputeReplay::Replay } else { ComputeReplay::NotCandidate }
}

/// An error a consumer inherits from a failed dependency (an input fence or
/// a firmware-ordered parent) is not the consumer's own device loss. With
/// INNOCENT set, the device-loss codes EIO, ETIMEDOUT and ENODEV become
/// ECANCELED for the consumer: its job fails, its VM keeps running. Errnos
/// are negative, as `Error::to_errno` returns them.
pub(crate) const fn inherited_errno(mask: u32, errno: i32) -> i32 {
    const EIO: i32 = -5;
    const ENODEV: i32 = -19;
    const ETIMEDOUT: i32 = -110;
    const ECANCELED: i32 = -125;
    if mask & INNOCENT != 0 && (errno == EIO || errno == ETIMEDOUT || errno == ENODEV) {
        ECANCELED
    } else {
        errno
    }
}

/// One more publication of a command that has been replayed `replays` times.
pub(crate) const fn replay_budget_left(replays: u8) -> bool { replays < MAX_REPLAYS }

/// Per-completion classification, written only while servicing a recovery.
pub(crate) const CLASS_NONE: u8 = 0;
/// Cancelled by another client's recovery (or graced): fail the fence only.
pub(crate) const CLASS_SPARED: u8 = 1;
/// Owned by the guilty VM: today's behaviour.
pub(crate) const CLASS_GUILTY: u8 = 2;

pub(crate) const ENTRY_VALID: u8 = 1 << 0;
pub(crate) const ENTRY_GUILTY: u8 = 1 << 1;
pub(crate) const ENTRY_CANCELLED: u8 = 1 << 2;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct RecoveryEntry {
    pub(crate) qid: u8,
    pub(crate) flags: u8,
    pub(crate) stamp: u32,
}

/// Decode one recovery-info table entry (see the module comment).
pub(crate) const fn decode_entry(word0: u32, word1: u32) -> Option<RecoveryEntry> {
    if word0 & 1 == 0 { return None; }
    // Native: ubfx #4, #7.
    let qid = (word0 >> 4) & 0x7f;
    Some(RecoveryEntry { qid: qid as u8, flags: (word0 & 0xf) as u8, stamp: word1 >> 8 })
}

/// QID masks of a decoded table plus the blamed stamp:
/// (guilty, listed, cancelled). The blamed stamp's QID counts as guilty.
pub(crate) fn qid_masks(entries: &[RecoveryEntry], blamed: Option<u8>) -> (u128, u128, u128) {
    let mut guilty = 0u128;
    let mut listed = 0u128;
    let mut cancelled = 0u128;
    for entry in entries {
        let bit = 1u128 << (entry.qid & 0x7f);
        listed |= bit;
        if entry.flags & ENTRY_GUILTY != 0 { guilty |= bit; }
        if entry.flags & ENTRY_CANCELLED != 0 { cancelled |= bit; }
    }
    if let Some(qid) = blamed.filter(|qid| *qid < 128) { guilty |= 1u128 << qid; }
    (guilty, listed, cancelled)
}

/// Class of one VM for one recovery. Returns (class, grace consumed).
pub(crate) const fn classify(guilty: bool, grace: bool, now_ns: u64, last_grace_ns: u64) -> (u8, bool) {
    if !guilty { return (CLASS_SPARED, false); }
    if grace && (last_grace_ns == 0 || now_ns.saturating_sub(last_grace_ns) >= GRACE_WINDOW_NS) {
        return (CLASS_SPARED, true);
    }
    (CLASS_GUILTY, false)
}

// ---- head-of-line victim deferral (bit7) ------------------------------------

/// The oldest unretired kick of one OTHER compute queue at a host timeout.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct ComputeHead {
    /// VM of that queue's owner.
    pub(crate) vm: u64,
    /// Global publication order of the kick (`COMPUTE_PUBLICATION`).
    pub(crate) published: u64,
    /// Its completion record was already latched (finished, not retired yet).
    pub(crate) finished: bool,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum VictimTimeout {
    /// No older unfinished kick of another VM: the timeout is this queue's own.
    Own,
    /// Head-of-line blocked: renew the scheduler timeout once more.
    Defer,
    /// Still blocked after the renewal budget: quarantine spared.
    Spare,
}

/// `own_vm`/`own_published`: the timed-out queue's VM and its oldest
/// unretired kick's publication order; `renewals`: periods already granted.
/// Returns the decision and the publication order of the oldest blocker.
pub(crate) fn victim_timeout<I>(mask: u32, own_vm: u64, own_published: u64, renewals: u8, others: I)
    -> (VictimTimeout, Option<u64>)
where I: IntoIterator<Item = ComputeHead> {
    if mask & VICTIM_DEFER == 0 { return (VictimTimeout::Own, None); }
    let blocker = others.into_iter()
        .filter(|head| head.vm != own_vm && !head.finished && head.published < own_published)
        .map(|head| head.published)
        .min();
    match blocker {
        None => (VictimTimeout::Own, None),
        Some(_) if renewals < VICTIM_MAX_RENEWALS => (VictimTimeout::Defer, blocker),
        // Sparing needs the INNOCENT machinery (no-poison fail, witness return).
        Some(_) if mask & INNOCENT != 0 => (VictimTimeout::Spare, blocker),
        Some(_) => (VictimTimeout::Own, blocker),
    }
}

/// Global publication order of async compute kicks. Written only under the
/// runtime lock at publication; a plain counter, never reset.
pub(crate) static COMPUTE_PUBLICATION: AtomicU64 = AtomicU64::new(0);

// ---- fault-source attribution (bit5) ---------------------------------------

/// One hardware slot of the recovery's fault mask, sampled while firmware is
/// halted: the slot's cached key (qid << 40 | stamp), its DM1 progress-monitor
/// state (0 = never written), and whether its pass had started (None = not a
/// render pass the host can resolve).
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct FaultSource {
    pub(crate) slot: u8,
    pub(crate) qid: Option<u8>,
    pub(crate) stamp: u64,
    pub(crate) monitor_state: u32,
    pub(crate) started: Option<bool>,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Attribution {
    /// Keep the firmware's guilty set (blamed stamp + table bit1).
    Firmware,
    /// The blamed slot never ran; these QIDs' owners are guilty instead.
    Reattributed(u128),
    /// The blamed slot never ran and no other source shows execution.
    NoCulprit,
}

pub(crate) const fn decode_slot_key(key: u64) -> (Option<u8>, u64) {
    if key == 0 { return (None, 0); }
    let qid = (key >> 40) & 0xff;
    (if qid < 128 { Some(qid as u8) } else { None }, key & 0xff_ffff_ffff)
}

/// `blamed_qid` is the QID of the firmware's stamp-blamed request. Across 13
/// saved recoveries the blamed slot is the lowest faulting slot of the first
/// data master, in TA, 3D, CDM order, that has one (e.g. 06:02:19 mask 0x11
/// blamed TA slot 0 over 3D slot 4; 03:18:16 mask 0x3 blamed 3D slot 1 over
/// compute slot 0) -- an inference from captures, so the blamed source is
/// matched by QID, never by bit position.
pub(crate) fn attribute(reason: u32, blamed_qid: Option<u8>, mask: u32, sources: &[FaultSource]) -> Attribution {
    // Page faults name the faulting VM directly; one source leaves no choice.
    if reason == 3 || mask.count_ones() < 2 { return Attribution::Firmware; }
    let Some(blamed_qid) = blamed_qid else { return Attribution::Firmware; };
    let Some(blamed) = sources.iter().find(|source| source.qid == Some(blamed_qid)) else {
        return Attribution::Firmware;
    };
    let blamed_slot = blamed.slot;
    if blamed.started != Some(false) || blamed.monitor_state != 0 {
        return Attribution::Firmware;
    }
    let mut qids = 0u128;
    // A culprit must have provably run: its pass start is stamped. A DM1
    // progress record alone is not used for a culprit (records are historical
    // DRAM and slots are shared across data masters, so a record may be stale).
    for source in sources.iter().filter(|source| source.slot != blamed_slot) {
        if source.started != Some(true) { continue; }
        if let Some(qid) = source.qid { qids |= 1u128 << (qid & 0x7f); }
    }
    if qids != 0 { Attribution::Reattributed(qids) } else { Attribution::NoCulprit }
}

pub(crate) const MAX_SOURCES: usize = 16;
static SRC_MASK: AtomicU32 = AtomicU32::new(0);
static SRC_REASON: AtomicU32 = AtomicU32::new(0);
static SRC_KEY: [AtomicU64; MAX_SOURCES] = [const { AtomicU64::new(0) }; MAX_SOURCES];
static SRC_MONITOR: [AtomicU32; MAX_SOURCES] = [const { AtomicU32::new(0) }; MAX_SOURCES];

/// Store the halted-firmware sample (`keys`/`monitors` indexed by slot).
pub(crate) fn store_sources(reason: u32, mask: u32, keys: &[u64; MAX_SOURCES], monitors: &[u32; MAX_SOURCES]) {
    for slot in 0..MAX_SOURCES {
        SRC_KEY[slot].store(keys[slot], Ordering::Relaxed);
        SRC_MONITOR[slot].store(monitors[slot], Ordering::Relaxed);
    }
    SRC_REASON.store(reason, Ordering::Relaxed);
    SRC_MASK.store(mask, Ordering::Release);
}

pub(crate) fn clear_sources() { SRC_MASK.store(0, Ordering::Release); }

/// (reason, mask, sources without `started`) of the last stored sample.
pub(crate) fn sources(out: &mut [FaultSource; MAX_SOURCES]) -> (u32, u32, usize) {
    let mask = SRC_MASK.load(Ordering::Acquire);
    let reason = SRC_REASON.load(Ordering::Relaxed);
    let mut used = 0;
    for slot in 0..MAX_SOURCES {
        if mask & (1u32 << slot) == 0 { continue; }
        let (qid, stamp) = decode_slot_key(SRC_KEY[slot].load(Ordering::Relaxed));
        out[used] = FaultSource {
            slot: slot as u8, qid, stamp,
            monitor_state: SRC_MONITOR[slot].load(Ordering::Relaxed), started: None,
        };
        used += 1;
    }
    (reason, mask, used)
}

// ---- recovery-info snapshot ------------------------------------------------

pub(crate) const MAX_ENTRIES: usize = 64;
static ENTRY_WORDS: [AtomicU64; MAX_ENTRIES] = [const { AtomicU64::new(0) }; MAX_ENTRIES];
static ENTRY_COUNT: AtomicU32 = AtomicU32::new(0);

/// Forget the previous recovery's table before reading the new one.
pub(crate) fn clear_entries() { ENTRY_COUNT.store(0, Ordering::Release); }

/// Store the raw (word0, word1) of every VALID entry, in table order.
pub(crate) fn store_entries(raw: &[(u32, u32)]) {
    let count = raw.len().min(MAX_ENTRIES);
    for (slot, &(word0, word1)) in ENTRY_WORDS.iter().zip(raw.iter()) {
        slot.store(u64::from(word0) | u64::from(word1) << 32, Ordering::Relaxed);
    }
    ENTRY_COUNT.store(count as u32, Ordering::Release);
}

/// Decoded entries of the last stored table.
pub(crate) fn entries(out: &mut [RecoveryEntry; MAX_ENTRIES]) -> usize {
    let count = (ENTRY_COUNT.load(Ordering::Acquire) as usize).min(MAX_ENTRIES);
    let mut used = 0;
    for slot in ENTRY_WORDS.iter().take(count) {
        let raw = slot.load(Ordering::Relaxed);
        if let Some(entry) = decode_entry(raw as u32, (raw >> 32) as u32) {
            out[used] = entry;
            used += 1;
        }
    }
    used
}

// ---- counters ----------------------------------------------------------------

pub(crate) static RECOVERIES: AtomicU64 = AtomicU64::new(0);
pub(crate) static UNRESOLVED: AtomicU64 = AtomicU64::new(0);
pub(crate) static SPARED_RENDERS: AtomicU64 = AtomicU64::new(0);
pub(crate) static SPARED_COMPUTE: AtomicU64 = AtomicU64::new(0);
pub(crate) static GUILTY_MARKED: AtomicU64 = AtomicU64::new(0);
pub(crate) static GRACED: AtomicU64 = AtomicU64::new(0);
pub(crate) static POISON_SKIPPED: AtomicU64 = AtomicU64::new(0);
pub(crate) static FANOUT_SKIPPED: AtomicU64 = AtomicU64::new(0);
pub(crate) static SETTLED_CANCELLED: AtomicU64 = AtomicU64::new(0);
pub(crate) static QUEUES_RETURNED: AtomicU64 = AtomicU64::new(0);
pub(crate) static QUEUE_RETURN_FAILED: AtomicU64 = AtomicU64::new(0);
pub(crate) static FIRST_KICKS_OK: AtomicU64 = AtomicU64::new(0);
pub(crate) static FIRST_KICKS_FAILED: AtomicU64 = AtomicU64::new(0);
pub(crate) static ADMISSION_WAITS: AtomicU64 = AtomicU64::new(0);
pub(crate) static ADMISSION_TIMEOUTS: AtomicU64 = AtomicU64::new(0);
pub(crate) static SUBMIT_WAITS: AtomicU64 = AtomicU64::new(0);
pub(crate) static SUBMIT_TIMEOUTS: AtomicU64 = AtomicU64::new(0);
pub(crate) static RESUME_DOORBELLS: AtomicU64 = AtomicU64::new(0);
pub(crate) static COLLATERAL: AtomicU64 = AtomicU64::new(0);
pub(crate) static REATTRIBUTED: AtomicU64 = AtomicU64::new(0);
pub(crate) static NO_CULPRIT: AtomicU64 = AtomicU64::new(0);
/// iso3.1: unclassified renders queued behind a spared head, failed with it.
pub(crate) static SPARED_SUCCESSORS: AtomicU64 = AtomicU64::new(0);
/// iso4 REPLAY: spared never-started compute kicks published again, and
/// spared kicks that had to fail instead (ran, unprovable, or republish failed).
pub(crate) static REPLAYED_COMPUTE: AtomicU64 = AtomicU64::new(0);
pub(crate) static REPLAY_REFUSED_COMPUTE: AtomicU64 = AtomicU64::new(0);
/// iso4: spared renders failed after firmware retirement, split by whether
/// the pass ever started (pass-start stamp) -- the render replay candidates.
pub(crate) static SPARED_RENDER_UNSTARTED: AtomicU64 = AtomicU64::new(0);
pub(crate) static SPARED_RENDER_STARTED: AtomicU64 = AtomicU64::new(0);
/// iso4: replayed compute jobs given a fresh scheduler timeout, and compute
/// publications that waited for their own queue's witness/replay first.
pub(crate) static REPLAY_TIMEOUT_RENEWED: AtomicU64 = AtomicU64::new(0);
pub(crate) static REPLAY_ORDER_WAITS: AtomicU64 = AtomicU64::new(0);
pub(crate) static REPLAY_ORDER_TIMEOUTS: AtomicU64 = AtomicU64::new(0);
/// iso4: replays refused because a render had already been released
/// against a cancelled kick; consumer failures not charged to the VM.
pub(crate) static REPLAY_REFUSED_CONSUMER: AtomicU64 = AtomicU64::new(0);
pub(crate) static INHERITED_NOT_POISONED: AtomicU64 = AtomicU64::new(0);
/// bit7: compute timeouts renewed because another VM's older kick was still
/// unfinished, and queues quarantined spared after the renewal budget.
pub(crate) static VICTIM_DEFERRED: AtomicU64 = AtomicU64::new(0);
pub(crate) static VICTIM_SPARED: AtomicU64 = AtomicU64::new(0);

#[inline(always)]
pub(crate) fn count(counter: &AtomicU64) { counter.fetch_add(1, Ordering::Relaxed); }

static COUNTERS: [(&str, &AtomicU64); 33] = [
    ("recoveries", &RECOVERIES), ("unresolved", &UNRESOLVED),
    ("spared-renders", &SPARED_RENDERS), ("spared-compute", &SPARED_COMPUTE),
    ("guilty-marked", &GUILTY_MARKED), ("graced", &GRACED),
    ("poison-skipped", &POISON_SKIPPED), ("fanout-skipped", &FANOUT_SKIPPED),
    ("settled-cancelled", &SETTLED_CANCELLED), ("queues-returned", &QUEUES_RETURNED),
    ("queue-return-failed", &QUEUE_RETURN_FAILED), ("first-kicks-ok", &FIRST_KICKS_OK),
    ("first-kicks-failed", &FIRST_KICKS_FAILED), ("admission-waits", &ADMISSION_WAITS),
    ("admission-timeouts", &ADMISSION_TIMEOUTS), ("submit-waits", &SUBMIT_WAITS),
    ("submit-timeouts", &SUBMIT_TIMEOUTS), ("resume-doorbells", &RESUME_DOORBELLS),
    ("collateral", &COLLATERAL), ("reattributed", &REATTRIBUTED), ("no-culprit", &NO_CULPRIT),
    ("spared-successors", &SPARED_SUCCESSORS),
    ("replayed-compute", &REPLAYED_COMPUTE), ("replay-refused-compute", &REPLAY_REFUSED_COMPUTE),
    ("spared-render-unstarted", &SPARED_RENDER_UNSTARTED), ("spared-render-started", &SPARED_RENDER_STARTED),
    ("replay-timeout-renewed", &REPLAY_TIMEOUT_RENEWED), ("replay-order-waits", &REPLAY_ORDER_WAITS),
    ("replay-order-timeouts", &REPLAY_ORDER_TIMEOUTS),
    ("replay-refused-consumer", &REPLAY_REFUSED_CONSUMER),
    ("inherited-not-poisoned", &INHERITED_NOT_POISONED),
    ("victim-deferred", &VICTIM_DEFERRED), ("victim-spared", &VICTIM_SPARED),
];

// ---- parameter -----------------------------------------------------------------

struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

pub(crate) fn parse_mask(text: &str) -> Option<u32> {
    let parsed = match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => text.parse::<u32>().ok(),
    }?;
    (parsed & !ALL_BITS == 0).then_some(parsed)
}

#[cfg(not(test))]
unsafe extern "C" fn mask_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    // SAFETY: the parameter core supplies a NUL-terminated string.
    let Ok(text) = unsafe { core::ffi::CStr::from_ptr(value) }.to_str() else { return -22; };
    let text = text.trim();
    if text == "reset" {
        for (_, counter) in COUNTERS.iter() { counter.store(0, Ordering::Relaxed); }
        return 0;
    }
    let Some(mask) = parse_mask(text) else { return -22; };
    MASK.store(mask, Ordering::Release);
    0
}

#[cfg(not(test))]
unsafe extern "C" fn mask_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes, used: 0 };
    let _ = write!(out, "{:#x}", MASK.load(Ordering::Relaxed));
    for (name, counter) in COUNTERS.iter() {
        let _ = write!(out, " {}={}", name, counter.load(Ordering::Relaxed));
    }
    let _ = writeln!(out);
    out.used as core::ffi::c_int
}

#[cfg(not(test))]
static MASK_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(mask_set), get: Some(mask_get), free: None,
};
#[cfg(not(test))]
#[link_section = "__param"]
#[used]
static MASK_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_recovery_isolation"
    } else { c"asahi.g17p_recovery_isolation" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&MASK_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
