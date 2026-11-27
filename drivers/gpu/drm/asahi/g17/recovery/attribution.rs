// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Resolve ambiguous recovery blame using slot identity and render execution evidence.

pub(super) const SLOTS: usize = 16;
const STAMP_MASK: u64 = (1 << 40) - 1;
/// Firmware recovery reason for a GPU MMU fault.
pub(crate) const PAGE_FAULT: u32 = 3;
/// M5 firmware recovery reason for a scheduling pause the GPU did not acknowledge in time. It was
/// read from every recovery of a ten-minute mixed client load on a J716s with the 27.3 beta 1
/// firmware.
pub(crate) const SCHEDULING_PAUSE_TIMEOUT: u32 = 5;
/// M5 firmware recovery reason for a GPU that did not reach forced idle after a sleep request.
/// Not yet observed.
pub(crate) const SLEEP_FORCED_IDLE_TIMEOUT: u32 = 6;

/// What a firmware recovery reason means: 3 a GPU page fault (as on G17P), 5 a scheduling pause
/// that timed out, 6 a forced idle that timed out after a sleep request. Reasons 0 to 4 are not
/// named here. Only 5 has been read from a running M5 firmware; 3 and 6 are
/// not yet confirmed from captured InitData.
pub(crate) fn cause(reason: u32) -> &'static str {
    match reason {
        PAGE_FAULT => "GPU MMU page fault",
        SCHEDULING_PAUSE_TIMEOUT => {
            "scheduling pause timeout: the GPU did not pause scheduling in time"
        }
        SLEEP_FORCED_IDLE_TIMEOUT => "sleep forced-idle timeout",
        0..=4 => "unnamed firmware reason",
        _ => "unknown firmware reason",
    }
}

/// The M5 firmware's recovery record in the primary status B (`M5_RECOVERY_REASON` and its
/// neighbours), read while the firmware waits in recovery state 1.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Record {
    pub(crate) reason: u32,
    pub(crate) slots_walked: u32,
    pub(crate) slot_word: u32,
    pub(crate) detail: u64,
}

#[derive(Clone, Copy, Default)]
pub(super) struct Source {
    slot: u8,
    qid: Option<u8>,
    stamp: u64,
    monitor: u32,
    started: Option<bool>,
}

pub(crate) struct Sources {
    /// The reason attribution uses. M5 leaves it unknown unless an MMU fault is latched; its
    /// firmware's own word is in `record`, which only the log reads.
    pub(crate) reason: Option<u32>,
    /// The M5 firmware's recovery record, for the log.
    pub(crate) record: Option<Record>,
    mask: u32,
    records: [Source; SLOTS],
    count: usize,
    /// `reason` reads PAGE_FAULT, but no MMU fault bank holds a fault (`check_fault_banks`).
    stale_fault: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Attribution {
    /// Only the firmware's blame: unconfirmed.
    Firmware,
    /// Evidence names these queues: slot identity, a latched fault, or a VM an earlier recovery
    /// found guilty.
    Queues(u128),
    /// The host's pass records suspect these queues (the oldest running render): unconfirmed,
    /// like the firmware's blame.
    Suspects(u128),
    NoCulprit,
}

impl Sources {
    /// Bounded evidence from the existing halted-firmware snapshot. No guessed
    /// MMIO/key addresses or additional firmware reads are used for diagnostics.
    pub(super) fn report(&self, generation: u64, stampless_total: u64, blamed: Option<u8>) {
        kernel::pr_warn!(
            "G17: recovery snapshot gen {} reason {:?} slot_mask {:#x} sampled {} known_keys {}; stampless lifetime total {} before restart\n",
            generation, self.reason, self.mask, self.count,
            self.records[..self.count].iter().filter(|source| source.qid.is_some()).count(),
            stampless_total
        );
        match self.record {
            Some(record) if record == Record::default() && self.mask == 0 => kernel::pr_warn!(
                "G17: recovery cause gen {}: none recorded (the record reads all zero); firmware blames queue {:?}\n",
                generation, blamed
            ),
            Some(record) => kernel::pr_warn!(
                "G17: recovery cause gen {}: {} (firmware reason {}); firmware blames queue {:?}; slots walked {}, slot word {:#x}, detail {:#x}\n",
                generation, cause(record.reason), record.reason, blamed, record.slots_walked,
                record.slot_word, record.detail
            ),
            None => {}
        }
        for source in &self.records[..self.count] {
            kernel::pr_warn!(
                "G17: recovery slot {} queue {:?} kick {:#x} monitor {:#x}\n",
                source.slot, source.qid, source.stamp, source.monitor
            );
        }
    }

    /// Firmware is halted for every selected key and progress-record read.
    /// Failed reads stay unknown; incomplete evidence cannot override firmware blame.
    pub(crate) fn sample(mask: u32, mut read: impl FnMut(usize) -> Option<(u64, u32)>) -> Self {
        let mut sources = Self {
            reason: None,
            record: None,
            mask,
            records: [Source::default(); SLOTS],
            count: 0,
            stale_fault: false,
        };
        for slot in 0..SLOTS {
            if mask & (1 << slot) == 0 {
                continue;
            }
            let Some((key, monitor)) = read(slot) else {
                continue;
            };
            let qid = (key >> 40) & 0xff;
            sources.records[sources.count] = Source {
                slot: slot as u8,
                qid: (key != 0 && qid < 128).then_some(qid as u8),
                stamp: key & STAMP_MASK,
                monitor,
                started: None,
            };
            sources.count += 1;
        }
        sources
    }

    /// The firmware's reason: the one attribution uses, else the M5 record's.
    pub(super) fn firmware_reason(&self) -> Option<u32> {
        self.reason.or(self.record.map(|record| record.reason))
    }

    /// Completes the reason of `InitData::fault_sources`. M5 attribution leaves the firmware
    /// reason out: its latched MMU fault register (`mmu_fault`) gives reason 3. A reason read
    /// from the firmware's reason word is checked against the MMU fault banks (`banks`, read
    /// while the firmware is halted) on every firmware ([`Self::check_fault_banks`]).
    pub(crate) fn settle_reason(&mut self, mmu_fault: bool, banks: impl FnOnce() -> Option<[(u64, u64); 2]>) {
        if self.reason.is_some() {
            if let Some(banks) = banks() {
                self.check_fault_banks(&banks);
            }
        } else if mmu_fault {
            self.reason = Some(PAGE_FAULT);
        }
    }

    /// The firmware writes its reason word only for some restarts, and nothing clears it: after
    /// a page fault, every later restart it gives no reason for still reads PAGE_FAULT
    /// (2026-11-18, 25G83: a deqp-vk spin read 0 before the boot's first page fault, 3 after
    /// it). A page fault keeps its access latched in an MMU fault bank until the restart, so
    /// when both banks read clear the reason is an earlier restart's and this one is no page
    /// fault.
    fn check_fault_banks(&mut self, banks: &[(u64, u64); 2]) {
        if self.reason == Some(PAGE_FAULT)
            && banks.iter().all(|&(status, address)| crate::regs::decode_fault(status, address, true).is_none())
        {
            kernel::pr_warn!("G17: recovery reason {} without a latched MMU fault is left over from an earlier restart\n",
                PAGE_FAULT);
            self.stale_fault = true;
        }
    }

    fn page_fault(&self) -> bool {
        self.reason == Some(PAGE_FAULT) && !self.stale_fault
    }

    pub(super) fn attribute(
        &mut self,
        blamed: Option<u8>,
        mut started: impl FnMut(u8, u64) -> Option<bool>,
    ) -> Attribution {
        if self.reason.is_none()
            || self.count as u32 != self.mask.count_ones()
            || self.records[..self.count].iter().any(|source| source.qid.is_none())
        {
            return Attribution::Firmware;
        }
        for source in &mut self.records[..self.count] {
            if let Some(qid) = source.qid {
                source.started = started(qid, source.stamp);
            }
        }
        if self.page_fault() || self.mask.count_ones() < 2 {
            return Attribution::Firmware;
        }
        let Some(blamed) = blamed else {
            return Attribution::Firmware;
        };
        let Some(source) = self.records[..self.count]
            .iter()
            .find(|source| source.qid == Some(blamed))
        else {
            return Attribution::Firmware;
        };
        if source.started != Some(false) || source.monitor != 0 {
            return Attribution::Firmware;
        }
        let slot = source.slot;
        let mut guilty = 0;
        for source in &self.records[..self.count] {
            if source.slot != slot && source.started == Some(true) {
                if let Some(qid) = source.qid {
                    guilty |= 1u128 << qid;
                }
            }
        }
        if guilty == 0 {
            Attribution::NoCulprit
        } else {
            Attribution::Queues(guilty)
        }
    }

    /// When slot identity cannot check a timeout's blame (25G83 reads every key as 0, and a
    /// single-slot mask has no peer), the host's own pass records still can: a blamed render
    /// that never started was starved, not hung. `culprit` gives their attribution
    /// (`Queues::render_culprit`); without one the firmware's blame stands.
    pub(super) fn host_fallback(
        &self,
        attribution: Attribution,
        culprit: impl FnOnce() -> Option<Attribution>,
    ) -> Attribution {
        if attribution != Attribution::Firmware || self.reason.is_none() || self.page_fault() {
            return attribution;
        }
        culprit().unwrap_or(Attribution::Firmware)
    }

    /// A page fault's MMU record names the UAT slots of the faulting accesses. Firmware blames
    /// a queue it chose, which can be a bystander's (a compute write fault blamed on a render
    /// queue of another VM). When the in-flight work of those slots (`culprit`) does not include
    /// the blamed queue, that work is blamed instead; without such evidence the firmware's blame
    /// stands.
    pub(super) fn fault_override(
        &self,
        attribution: Attribution,
        blamed: Option<u8>,
        slots: u64,
        culprit: impl FnOnce(u64) -> Option<u128>,
    ) -> Attribution {
        if !self.page_fault() || attribution != Attribution::Firmware || slots == 0 {
            return attribution;
        }
        let Some(blamed) = blamed else {
            return attribution;
        };
        match culprit(slots) {
            Some(qids) if qids != 0 && qids & (1u128 << blamed) == 0 => Attribution::Queues(qids),
            _ => attribution,
        }
    }
}

/// The QIDs of every queue with an in-flight packet in one of the UAT `slots`. `visit` reports
/// each in-flight packet as its queue's QIDs and its context ID (the UAT slot it runs in).
pub(crate) fn fault_qids(slots: u64, visit: impl FnOnce(&mut dyn FnMut(u128, u32))) -> Option<u128> {
    let mut qids = 0;
    visit(&mut |queue, context| {
        if 1u64.checked_shl(context).is_some_and(|bit| bit & slots != 0) {
            qids |= queue;
        }
    });
    (qids != 0).then_some(qids)
}

#[kernel::macros::kunit_tests(asahi_g17_recovery_sources)]
mod tests {
    use super::*;

    #[test]
    fn progress_without_keys_preserves_firmware_blame() {
        let mut reads = 0;
        let mut sources = Sources::sample(0b101, |slot| {
            reads += 1;
            Some((0, (slot + 1) as u32))
        });
        sources.reason = Some(2);
        assert_eq!(reads, 2);
        assert_eq!(sources.count, 2);
        assert_eq!(sources.records[1].slot, 2);
        assert_eq!(sources.records[1].monitor, 3);
        assert!(sources.records[..sources.count].iter().all(|source| source.qid.is_none()));
        assert_eq!(sources.attribute(Some(38), |_, _| panic!("unknown key")),
            Attribution::Firmware);
    }

    #[test]
    fn incomplete_progress_cannot_clear_firmware_blame() {
        let mut sources = Sources::sample(0b11, |slot| {
            (slot == 0).then_some(((38 << 40) | 1, 0))
        });
        sources.reason = Some(2);
        assert_eq!(sources.attribute(Some(38), |_, _| Some(false)), Attribution::Firmware);
    }

    #[test]
    fn a_missing_peer_key_cannot_clear_a_known_queues_blame() {
        let mut sources = Sources::sample(0b11, |slot| {
            Some((if slot == 0 { (38 << 40) | 1 } else { 0 }, 0))
        });
        sources.reason = Some(2);
        assert_eq!(sources.count, 2);
        assert_eq!(sources.attribute(Some(38), |_, _| Some(false)), Attribution::Firmware);
    }

    #[test]
    fn page_fault_keeps_firmware_blame_with_complete_keys() {
        let mut sources = Sources::sample(0b11, |slot| {
            Some(((((38 + slot) as u64) << 40) | 1, 0))
        });
        sources.reason = Some(PAGE_FAULT);
        assert_eq!(sources.attribute(Some(38), |qid, _| Some(qid == 39)),
            Attribution::Firmware);
    }

    #[test]
    fn host_records_move_a_keyless_timeout_blame_to_the_running_vm() {
        let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
        sources.reason = Some(0);
        let attribution = sources.attribute(Some(36), |_, _| panic!("unknown key"));
        assert_eq!(attribution, Attribution::Firmware);
        assert_eq!(sources.host_fallback(attribution, || Some(Attribution::Suspects(3 << 60))), Attribution::Suspects(3 << 60));
        assert_eq!(sources.host_fallback(attribution, || None), Attribution::Firmware);
        // The host's records clear every running render (the blamed one never started).
        assert_eq!(sources.host_fallback(attribution, || Some(Attribution::NoCulprit)), Attribution::NoCulprit);
    }

    #[test]
    fn m5_record_names_the_cause_without_steering_blame() {
        let mut sources = Sources::sample(0b101, |_| None);
        sources.record = Some(Record {
            reason: SCHEDULING_PAUSE_TIMEOUT,
            ..Record::default()
        });
        assert_eq!(sources.firmware_reason(), Some(SCHEDULING_PAUSE_TIMEOUT));
        assert_eq!(
            cause(SCHEDULING_PAUSE_TIMEOUT),
            "scheduling pause timeout: the GPU did not pause scheduling in time"
        );
        assert_eq!(
            sources.attribute(Some(38), |_, _| panic!("no keys")),
            Attribution::Firmware
        );
        assert_eq!(
            sources.host_fallback(Attribution::Firmware, || panic!("reason unused")),
            Attribution::Firmware
        );
    }

    #[test]
    fn host_records_never_override_a_page_fault_or_slot_evidence() {
        let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
        sources.reason = Some(PAGE_FAULT);
        assert_eq!(sources.host_fallback(Attribution::Firmware, || panic!("page fault")),
            Attribution::Firmware);
        sources.reason = None;
        assert_eq!(sources.host_fallback(Attribution::Firmware, || panic!("no reason")),
            Attribution::Firmware);
        sources.reason = Some(0);
        assert_eq!(sources.host_fallback(Attribution::NoCulprit, || panic!("slot evidence")),
            Attribution::NoCulprit);
    }

    #[test]
    fn a_page_fault_reason_without_a_latched_fault_is_judged_like_a_timeout() {
        // 2026-11-18 16:39:57 UTC: deqp-vk's spinning renders (queues 84/85, failed by the
        // first restart) kept hanging the GPU; the reason word still read the page fault of
        // 04:30, both banks were clear, and the firmware blamed T3 Code's queue 40, then
        // Hyprland's 36. Both lost their device.
        let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
        sources.reason = Some(PAGE_FAULT);
        sources.check_fault_banks(&[(0, 0); 2]);
        let attribution = sources.attribute(Some(40), |_, _| panic!("unknown key"));
        assert_eq!(attribution, Attribution::Firmware);
        assert_eq!(sources.host_fallback(attribution, || Some(Attribution::Queues(3 << 84))), Attribution::Queues(3 << 84));
        assert_eq!(sources.fault_override(attribution, Some(40), 0, |_| panic!("no fault")),
            Attribution::Firmware);
    }

    #[test]
    fn only_a_reason_from_the_firmware_word_is_checked_against_the_banks() {
        // Any firmware's reason word: clear banks show a left-over page fault.
        let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
        sources.reason = Some(PAGE_FAULT);
        sources.settle_reason(false, || Some([(0, 0); 2]));
        assert!(sources.stale_fault);
        // Unreadable banks leave it a page fault.
        let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
        sources.reason = Some(PAGE_FAULT);
        sources.settle_reason(false, || None);
        assert!(sources.page_fault());
        // M5 has no reason word: its latched fault register is the page fault, never judged
        // by the banks.
        let mut sources = Sources::sample(0b10, |_| None);
        sources.settle_reason(true, || panic!("M5 fault checked against the banks"));
        assert!(sources.page_fault());
        // No reason and no latched fault stays unknown.
        let mut sources = Sources::sample(0b10, |_| None);
        sources.settle_reason(false, || panic!("no reason to check"));
        assert_eq!(sources.reason, None);
    }

    #[test]
    fn a_latched_fault_in_either_bank_keeps_the_page_fault() {
        // 2026-11-17 18:27:07 UTC: reason 3, bank 0 clear, bank 1 an unmapped UL1C read.
        for banks in [[(0, 0), (0x84e926e311, 0x40f00003d80 >> 6)], [(0x8f0779c411, 0), (0, 0)]] {
            let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
            sources.reason = Some(PAGE_FAULT);
            sources.check_fault_banks(&banks);
            assert_eq!(sources.host_fallback(Attribution::Firmware, || panic!("page fault")),
                Attribution::Firmware);
        }
        // Clear banks change no other reason.
        let mut sources = Sources::sample(0b10, |_| Some((0, 2)));
        sources.reason = Some(2);
        sources.check_fault_banks(&[(0, 0); 2]);
        assert!(!sources.stale_fault);
    }

    /// The in-flight work of 2026-11-06 12:37:58: WoW's render pair (QIDs 65 and 66, four
    /// renders in context 19) and a compute test's queue 60 in context 17.
    fn incident(slots: u64) -> Option<u128> {
        fault_qids(slots, |visit| {
            for _ in 0..4 {
                visit(3 << 65, 19);
            }
            visit(1 << 60, 17);
        })
    }

    #[test]
    fn a_page_fault_in_another_vm_moves_firmware_blame() {
        // Both MMU banks: write ReadOnly, units CMP(2)/CMP(0), vm_slot 17. Firmware blamed
        // queue 66; slot 5's key read 0, so slot identity could not check it.
        let mut sources = Sources::sample(1 << 5, |_| Some((0, 0)));
        sources.reason = Some(PAGE_FAULT);
        let attribution = sources.attribute(Some(66), |_, _| panic!("unknown key"));
        assert_eq!(attribution, Attribution::Firmware);
        let attribution = sources.host_fallback(attribution, || panic!("page fault"));
        assert_eq!(sources.fault_override(attribution, Some(66), 1 << 17, incident),
            Attribution::Queues(1 << 60));
    }

    #[test]
    fn a_page_fault_in_the_blamed_vm_keeps_firmware_blame() {
        let mut sources = Sources::sample(1 << 5, |_| Some((0, 0)));
        sources.reason = Some(PAGE_FAULT);
        assert_eq!(sources.fault_override(Attribution::Firmware, Some(66), 1 << 19, incident),
            Attribution::Firmware);
        // Both VMs faulted: the blamed queue is among the culprits.
        assert_eq!(sources.fault_override(Attribution::Firmware, Some(66), (1 << 17) | (1 << 19),
            incident), Attribution::Firmware);
    }

    #[test]
    fn a_page_fault_without_slot_evidence_keeps_firmware_blame() {
        let mut sources = Sources::sample(1 << 5, |_| Some((0, 0)));
        sources.reason = Some(PAGE_FAULT);
        // No decodable fault in either bank (reason 3 with clear status words).
        assert_eq!(sources.fault_override(Attribution::Firmware, Some(66), 0, |_| panic!("no fault")),
            Attribution::Firmware);
        // No in-flight work runs in the faulting slot.
        assert_eq!(sources.fault_override(Attribution::Firmware, Some(66), 1 << 23, incident),
            Attribution::Firmware);
        // Firmware blamed no queue: the recovery table alone decides, as before.
        assert_eq!(sources.fault_override(Attribution::Firmware, None, 1 << 17, |_| panic!("no blame")),
            Attribution::Firmware);
    }

    #[test]
    fn fault_slots_never_override_a_timeout_or_slot_evidence() {
        let mut sources = Sources::sample(1 << 5, |_| Some((0, 0)));
        sources.reason = Some(2);
        assert_eq!(sources.fault_override(Attribution::Firmware, Some(66), 1 << 17, |_| panic!("timeout")),
            Attribution::Firmware);
        sources.reason = Some(PAGE_FAULT);
        assert_eq!(sources.fault_override(Attribution::NoCulprit, Some(66), 1 << 17, |_| panic!("slots")),
            Attribution::NoCulprit);
        assert_eq!(sources.fault_override(Attribution::Queues(1 << 38), Some(66), 1 << 17,
            |_| panic!("slots")), Attribution::Queues(1 << 38));
    }

    #[test]
    fn fault_qids_match_contexts_by_uat_slot() {
        assert_eq!(incident(1 << 17), Some(1 << 60));
        assert_eq!(incident(1 << 19), Some(3 << 65));
        assert_eq!(incident(0), None);
        // A context ID beyond the mask's 64 slots never matches.
        assert_eq!(fault_qids(u64::MAX, |visit| visit(1 << 7, 64)), None);
    }
}
