// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Shared QoS publication and exact-completion accounting for every engine.
//!
//! The device publication lock serializes this host state. Publishing identity
//! precedes submitted counters; retiring a final owner clears only its two row
//! bytes. Cancellation balances host references without rewinding shared counts.

use super::{fw::qos as fw, object::KernelObject};
use core::{
    mem::{offset_of, size_of},
    sync::atomic::{fence, AtomicU32, AtomicU64, AtomicU8, Ordering},
};
use kernel::prelude::*;
use crate::hw::t8140::qos as cfg;

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
    pub(crate) fn new(object: &'a KernelObject) -> Result<Self> {
        let ptr = object.pointer(0, size_of::<fw::Qos>())?;
        if ptr as usize & 7 != 0 {
            return Err(EINVAL);
        }
        // SAFETY: pointer spans the complete layout, whose asserted offsets
        // and size bound every array below. Integer atomics have their integer
        // representation and alignment. The retained object borrow keeps every
        // shared reference alive; accesses use each field's fixed width.
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
    pub(crate) fn publish(
        &mut self,
        view: &View<'_>,
        owner: Owner,
        scheduler: u64,
        clock: impl FnOnce() -> (u64, u64),
    ) -> Result<Publication> {
        let (qid, qos, dm) = owner.indices()?;
        if scheduler == 0 {
            return Err(EINVAL);
        }
        let next_qid = self.in_flight[qid].checked_add(1).ok_or(EOVERFLOW)?;
        let next_dm = self.dm_in_flight[dm].checked_add(1).ok_or(EOVERFLOW)?;
        let starts_busy = self.dm_in_flight[dm] == 0;
        let time = if starts_busy {
            let (counter, frequency) = clock();
            time_from_host(counter, frequency)?
        } else {
            0
        };
        if self.in_flight[qid] != 0
            && (view.rows[qid][0].load(Ordering::Relaxed) != owner.qos
                || self.dm_by_qid[qid] != owner.data_master)
        {
            return Err(EBUSY);
        }
        view.rows[qid][0].store(owner.qos, Ordering::Relaxed);
        view.rows[qid][1].store(cfg::CLASS, Ordering::Relaxed);
        let record = view.row(qid);
        view.scheduler[qos].store(scheduler, Ordering::Relaxed);
        view.share[qos].store(cfg::SHARE, Ordering::Relaxed);
        if self.in_flight[qid] == 0 {
            fence(Ordering::SeqCst);
        }
        view.queue_submitted[qid].store(
            view.queue_submitted[qid]
                .load(Ordering::Relaxed)
                .wrapping_add(1),
            Ordering::Relaxed,
        );
        view.owner_submitted[qos].store(
            view.owner_submitted[qos]
                .load(Ordering::Relaxed)
                .wrapping_add(1),
            Ordering::Relaxed,
        );
        if starts_busy {
            view.times_and_counts[dm].store(time, Ordering::Relaxed);
        }
        let submitted = &view.times_and_counts[3 + dm];
        let dm_submitted = submitted.load(Ordering::Relaxed).wrapping_add(1);
        submitted.store(dm_submitted, Ordering::Relaxed);
        if starts_busy {
            view.intervals.store(
                view.intervals.load(Ordering::Relaxed).wrapping_add(1),
                Ordering::Relaxed,
            );
        }
        self.in_flight[qid] = next_qid;
        self.dm_in_flight[dm] = next_dm;
        self.dm_by_qid[qid] = owner.data_master;
        fence(Ordering::SeqCst);
        Ok(Publication {
            record,
            owner,
            dm_submitted,
        })
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

/// Firmware accounting uses a 24 MHz domain independent of host timer speed.
pub(crate) fn time_from_host(counter: u64, frequency: u64) -> Result<u64> {
    if frequency == 0 || frequency > u64::from(u32::MAX) {
        return Err(EINVAL);
    }
    let whole = (counter / frequency)
        .checked_mul(cfg::CLOCK_HZ)
        .ok_or(EOVERFLOW)?;
    let fraction = (counter % frequency) * cfg::CLOCK_HZ / frequency;
    whole.checked_add(fraction).ok_or(EOVERFLOW)
}

pub(crate) fn clock() -> (u64, u64) {
    let counter: u64;
    let frequency: u64;
    // SAFETY: Both timer registers are readable at EL1 and have no memory effects.
    unsafe {
        core::arch::asm!("mrs {counter}, CNTPCT_EL0", "mrs {frequency}, CNTFRQ_EL0",
        counter=out(reg)counter, frequency=out(reg)frequency, options(nomem,nostack,preserves_flags));
    }
    (counter, frequency)
}
