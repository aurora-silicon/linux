// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Shared QoS publication and exact-completion accounting for every engine.
//!
//! The port's QoS lock serializes this host state. Publishing identity
//! precedes submitted counters; retiring a final owner clears only its two row
//! bytes. Cancellation balances host references without rewinding shared counts.

use super::fw::{qos as fw, queue::Policy};
use crate::hw::t8140::qos as cfg;
use core::{
    mem::offset_of,
    sync::atomic::{fence, AtomicU32, AtomicU64, AtomicU8, Ordering},
};
use kernel::prelude::*;

/// Atomic views retain the allocation borrow and never borrow firmware words
/// as ordinary mutable memory. The arrays are disjoint apart from the explicit
/// data-master time/count indexing in `times_and_counts`.
pub(crate) struct View<'a> {
    rows: &'a [[AtomicU8; 8]; fw::QUEUES],
    queue_submitted: &'a [AtomicU32; fw::QUEUES],
    owner_submitted: &'a [AtomicU32; fw::QUEUES],
    scheduler: &'a [AtomicU64; fw::QUEUES],
    share: &'a [AtomicU32; fw::QUEUES],
    intervals: &'a AtomicU64,
    times_and_counts: &'a [AtomicU64; 7],
}
impl<'a> View<'a> {
    /// # Safety
    ///
    /// `ptr` must span a complete QoS table that stays mapped for `'a`.
    pub(crate) unsafe fn from_raw(ptr: *mut u8) -> Result<Self> {
        if ptr as usize & 7 != 0 {
            return Err(EINVAL);
        }
        // SAFETY: pointer spans the complete layout, whose asserted offsets
        // and size bound every array below. Integer atomics have their integer
        // representation and alignment. The caller keeps every shared
        // reference alive; accesses use each field's fixed width.
        Ok(unsafe {
            Self {
                rows: &*ptr.add(offset_of!(fw::Qos, rows)).cast(),
                queue_submitted: &*ptr.add(offset_of!(fw::Qos, queue_submitted)).cast(),
                owner_submitted: &*ptr.add(offset_of!(fw::Qos, owner_submitted)).cast(),
                scheduler: &*ptr.add(offset_of!(fw::Qos, scheduler)).cast(),
                share: &*ptr.add(offset_of!(fw::Qos, share)).cast(),
                intervals: &*ptr.add(offset_of!(fw::Qos, intervals)).cast(),
                times_and_counts: &*ptr.add(offset_of!(fw::Qos, busy_start)).cast(),
            }
        })
    }
    fn row(&self, qid: usize) -> [u8; 8] {
        core::array::from_fn(|index| self.rows[qid][index].load(Ordering::Relaxed))
    }
    /// The complete row in one access. The table is uncached: a publication reads
    /// each row once, before it stores anything.
    fn row_word(&self, qid: usize) -> [u8; 8] {
        let row = core::ptr::from_ref(&self.rows[qid]).cast::<AtomicU64>();
        // SAFETY: Rows are eight bytes at eight-byte offsets of the eight-byte
        // aligned table (checked in from_raw), so the row is one aligned word.
        unsafe { (*row).load(Ordering::Relaxed) }.to_le_bytes()
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Owner {
    pub(crate) qid: u8,
    pub(crate) qos: u8,
    pub(crate) data_master: u8,
}
impl Owner {
    fn indices(self) -> Result<(usize, usize, usize)> {
        let (qid, qos, dm) = (
            usize::from(self.qid),
            usize::from(self.qos),
            usize::from(self.data_master),
        );
        if qid >= fw::QUEUES || qos >= fw::QUEUES || dm >= fw::DATA_MASTERS {
            return Err(EINVAL);
        }
        Ok((qid, qos, dm))
    }
}

/// Retained until completion, or cancellation before the producer is exposed.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Publication {
    pub(crate) record: [u8; 8],
    owner: Owner,
    dm_submitted: u64,
}

pub(crate) struct Accounting {
    in_flight: [u32; fw::QUEUES],
    dm_in_flight: [u64; fw::DATA_MASTERS],
    dm_by_qid: [u8; fw::QUEUES],
}
impl Accounting {
    pub(crate) const fn new() -> Self {
        Self {
            in_flight: [0; fw::QUEUES],
            dm_in_flight: [0; fw::DATA_MASTERS],
            dm_by_qid: [0; fw::QUEUES],
        }
    }

    /// Called under the device publication lock immediately before exposing
    /// the work record. The clock is sampled only when its engine becomes busy.
    /// The row's class and the owner's share come from the owner's `policy`.
    pub(crate) fn publish(
        &mut self,
        view: &View<'_>,
        owner: Owner,
        scheduler: u64,
        policy: Policy,
        clock: Clock,
    ) -> Result<Publication> {
        let [publication] = self.publish_owners(view, [owner], scheduler, policy, clock)?;
        Ok(publication)
    }

    /// Publishes the owners of one command's queues, in order, with the same
    /// firmware-visible values as one `publish` per owner. The owners use
    /// distinct queues and data masters. Every check precedes the first store,
    /// so a refusal changes nothing; every table word is read before the first
    /// store, so no uncached read waits for an earlier store to drain.
    pub(crate) fn publish_owners<const N: usize>(
        &mut self,
        view: &View<'_>,
        owners: [Owner; N],
        scheduler: u64,
        policy: Policy,
        clock: Clock,
    ) -> Result<[Publication; N]> {
        if scheduler == 0 {
            return Err(EINVAL);
        }
        let mut indices = [(0, 0, 0); N];
        for (index, owner) in owners.iter().enumerate() {
            indices[index] = owner.indices()?;
            let (qid, _, dm) = indices[index];
            if indices[..index]
                .iter()
                .any(|&(other_qid, _, other_dm)| other_qid == qid || other_dm == dm)
            {
                return Err(EINVAL);
            }
            self.in_flight[qid].checked_add(1).ok_or(EOVERFLOW)?;
            self.dm_in_flight[dm].checked_add(1).ok_or(EOVERFLOW)?;
        }
        let rows = indices.map(|(qid, _, _)| view.row_word(qid));
        let queue_submitted = indices.map(|(qid, _, _)| view.queue_submitted[qid].load(Ordering::Relaxed));
        let owner_submitted = indices.map(|(_, qos, _)| view.owner_submitted[qos].load(Ordering::Relaxed));
        let dm_submitted = indices.map(|(_, _, dm)| view.times_and_counts[3 + dm].load(Ordering::Relaxed));
        let starts_busy = indices.map(|(_, _, dm)| self.dm_in_flight[dm] == 0);
        let busy = starts_busy.iter().any(|&starts| starts);
        let intervals = if busy { view.intervals.load(Ordering::Relaxed) } else { 0 };
        for (index, owner) in owners.iter().enumerate() {
            let (qid, _, _) = indices[index];
            if self.in_flight[qid] != 0
                && (rows[index][0] != owner.qos || self.dm_by_qid[qid] != owner.data_master)
            {
                return Err(EBUSY);
            }
        }
        let time = if busy { clock.firmware_time()? } else { 0 };
        let class = policy.qos_class();
        let mut first = false;
        for (index, owner) in owners.iter().enumerate() {
            let (qid, qos, _) = indices[index];
            view.rows[qid][0].store(owner.qos, Ordering::Relaxed);
            view.rows[qid][1].store(class, Ordering::Relaxed);
            view.scheduler[qos].store(scheduler, Ordering::Relaxed);
            view.share[qos].store(policy.qos_share(), Ordering::Relaxed);
            first |= self.in_flight[qid] == 0;
        }
        if first {
            fence(Ordering::SeqCst);
        }
        let mut publications = [Publication {
            record: [0; 8],
            owner: owners[0],
            dm_submitted: 0,
        }; N];
        let mut intervals = intervals;
        for (index, owner) in owners.iter().enumerate() {
            let (qid, qos, dm) = indices[index];
            view.queue_submitted[qid].store(queue_submitted[index].wrapping_add(1), Ordering::Relaxed);
            // Owners of one logical queue share their submission count.
            let earlier = indices[..index].iter().filter(|&&(_, other, _)| other == qos).count();
            view.owner_submitted[qos].store(
                owner_submitted[index].wrapping_add(earlier as u32 + 1),
                Ordering::Relaxed,
            );
            if starts_busy[index] {
                view.times_and_counts[dm].store(time, Ordering::Relaxed);
            }
            let submitted = dm_submitted[index].wrapping_add(1);
            view.times_and_counts[3 + dm].store(submitted, Ordering::Relaxed);
            if starts_busy[index] {
                intervals = intervals.wrapping_add(1);
                view.intervals.store(intervals, Ordering::Relaxed);
            }
            let mut record = rows[index];
            record[0] = owner.qos;
            record[1] = class;
            self.in_flight[qid] += 1;
            self.dm_in_flight[dm] += 1;
            self.dm_by_qid[qid] = owner.data_master;
            publications[index] = Publication {
                record,
                owner: *owner,
                dm_submitted: submitted,
            };
        }
        fence(Ordering::SeqCst);
        Ok(publications)
    }

    /// A possibly exposed producer must retain its reference until completion.
    /// A newer submission on the same engine makes an old ticket uncancellable.
    pub(crate) fn cancel(&mut self, view: &View<'_>, ticket: Publication) -> Result {
        let (qid, _, dm) = ticket.owner.indices()?;
        if self.in_flight[qid] == 0
            || self.dm_in_flight[dm] == 0
            || view.row(qid) != ticket.record
            || view.times_and_counts[3 + dm].load(Ordering::Relaxed) != ticket.dm_submitted
        {
            return Err(EIO);
        }
        self.complete(view, &[ticket.owner])
    }

    /// Preflights the entire set before clearing any final row. QID membership,
    /// scheduler mappings and monotonically increasing counts remain intact.
    pub(crate) fn complete(&mut self, view: &View<'_>, owners: &[Owner]) -> Result {
        let mut completed = [0u64; fw::DATA_MASTERS];
        for (index, owner) in owners.iter().copied().enumerate() {
            let (qid, _, dm) = owner.indices()?;
            if owners[..index].iter().any(|other| other.qid == owner.qid) {
                return Err(EINVAL);
            }
            if self.in_flight[qid] == 0
                || view.rows[qid][0].load(Ordering::Relaxed) != owner.qos
                || self.dm_by_qid[qid] != owner.data_master
            {
                return Err(EIO);
            }
            completed[dm] += 1;
        }
        for (dm, count) in completed.iter().enumerate() {
            if *count > self.dm_in_flight[dm] {
                return Err(EIO);
            }
        }
        for owner in owners {
            let qid = usize::from(owner.qid);
            if self.in_flight[qid] == 1 {
                view.rows[qid][0].store(0xff, Ordering::Relaxed);
                view.rows[qid][1].store(0, Ordering::Relaxed);
            }
        }
        for owner in owners {
            self.in_flight[usize::from(owner.qid)] -= 1;
        }
        for (dm, count) in completed.iter().enumerate() {
            self.dm_in_flight[dm] -= count;
        }
        fence(Ordering::SeqCst);
        Ok(())
    }
}

fn scale(counter: u64, frequency: u64, target: u64) -> Result<u64> {
    if frequency == 0 || frequency > u64::from(u32::MAX) {
        return Err(EINVAL);
    }
    let whole = (counter / frequency).checked_mul(target).ok_or(EOVERFLOW)?;
    let fraction = (counter % frequency) * target / frequency;
    whole.checked_add(fraction).ok_or(EOVERFLOW)
}

/// One architectural-counter sample shared by every timestamp use of a submission.
#[derive(Clone, Copy)]
pub(crate) struct Clock {
    counter: u64,
    frequency: u64,
    monotonic_ns: u64,
}

impl Clock {
    /// Firmware accounting uses a 24 MHz domain independent of host timer speed.
    fn firmware_time(self) -> Result<u64> {
        scale(self.counter, self.frequency, cfg::CLOCK_HZ)
    }

    pub(crate) fn monotonic_ns(self) -> u64 {
        self.monotonic_ns
    }
}

pub(crate) fn frequency() -> u64 {
    let frequency: u64;
    // SAFETY: The timer frequency register is readable at EL1 and has no memory effects.
    unsafe {
        core::arch::asm!("mrs {frequency}, CNTFRQ_EL0",
        frequency=out(reg)frequency, options(nomem,nostack,preserves_flags));
    }
    frequency
}

pub(crate) fn clock(frequency: u64, monotonic_ns: u64) -> Clock {
    let counter: u64;
    // SAFETY: The physical timer register is readable at EL1 and has no memory effects.
    unsafe {
        core::arch::asm!("mrs {counter}, CNTPCT_EL0",
        counter=out(reg)counter, options(nomem,nostack,preserves_flags));
    }
    Clock {
        counter,
        frequency,
        monotonic_ns,
    }
}

#[kernel::macros::kunit_tests(asahi_g17_qos_clock)]
mod clock_tests {
    use super::*;

    #[test]
    fn scales_one_sample_into_both_domains() {
        let clock = Clock {
            counter: 25_125_000,
            frequency: 25_000_000,
            monotonic_ns: 1_005_000_000,
        };
        assert_eq!(clock.firmware_time(), Ok(24_120_000));
        assert_eq!(clock.monotonic_ns(), 1_005_000_000);
    }

    #[test]
    fn rejects_an_invalid_frequency() {
        assert!(matches!(scale(1, 0, cfg::CLOCK_HZ), Err(EINVAL)));
        assert!(matches!(
            scale(1, u64::from(u32::MAX) + 1, cfg::CLOCK_HZ),
            Err(EINVAL)
        ));
    }
}
