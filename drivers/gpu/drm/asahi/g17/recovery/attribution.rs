// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Resolve ambiguous recovery blame using slot identity and render execution evidence.

pub(super) const SLOTS: usize = 16;
const STAMP_MASK: u64 = (1 << 40) - 1;
const PAGE_FAULT: u32 = 3;

#[derive(Clone, Copy, Default)]
pub(super) struct Source {
    slot: u8,
    qid: Option<u8>,
    stamp: u64,
    monitor: u32,
    started: Option<bool>,
}

pub(crate) struct Sources {
    pub(crate) reason: u32,
    mask: u32,
    records: [Source; SLOTS],
    count: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Attribution {
    Firmware,
    Queues(u128),
    NoCulprit,
}

impl Sources {
    /// Firmware is halted for every selected key and progress-record read.
    /// Missing keys stay unknown; they cannot establish execution by an owner.
    pub(crate) fn sample(mask: u32, mut read: impl FnMut(usize) -> (u64, u32)) -> Self {
        let mut sources = Self {
            reason: 0,
            mask,
            records: [Source::default(); SLOTS],
            count: 0,
        };
        for slot in 0..SLOTS {
            if mask & (1 << slot) == 0 {
                continue;
            }
            let (key, monitor) = read(slot);
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

    pub(super) fn attribute(
        &mut self,
        blamed: Option<u8>,
        mut started: impl FnMut(u8, u64) -> Option<bool>,
    ) -> Attribution {
        for source in &mut self.records[..self.count] {
            if let Some(qid) = source.qid {
                source.started = started(qid, source.stamp);
            }
        }
        if self.reason == PAGE_FAULT || self.mask.count_ones() < 2 {
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
}
