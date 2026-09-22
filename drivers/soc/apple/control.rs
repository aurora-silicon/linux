// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use crate::proto;
use kernel::prelude::*;

const MAX_INFLIGHT: usize = 8;

const POOL_LEN: usize = (proto::TAG_POOL_LAST - proto::TAG_POOL_FIRST + 1) as usize;

#[derive(Clone, Copy)]
struct Slot {
    used: bool,
    tag: u8,
    reply: Option<proto::ControlReply>,
}

impl Slot {
    const FREE: Slot = Slot {
        used: false,
        tag: 0,
        reply: None,
    };
}

struct EntropySink {
    value: Option<u32>,
    busy: bool,
}

pub(crate) enum Delivery {
    Entropy,
    Matched,
    Unmatched,
}

pub(crate) struct ControlState {
    slots: [Slot; MAX_INFLIGHT],
    next_tag: u8,
    retired: [u64; 4],
    retired_count: u32,
    entropy: EntropySink,
}

impl ControlState {
    pub(crate) fn new() -> Self {
        ControlState {
            slots: [Slot::FREE; MAX_INFLIGHT],
            next_tag: proto::TAG_POOL_FIRST,
            retired: [0; 4],
            retired_count: 0,
            entropy: EntropySink {
                value: None,
                busy: false,
            },
        }
    }

    fn is_retired(&self, tag: u8) -> bool {
        self.retired[(tag >> 6) as usize] & (1u64 << (tag & 0x3f)) != 0
    }

    fn retire(&mut self, tag: u8) {
        if !self.is_retired(tag) {
            self.retired[(tag >> 6) as usize] |= 1u64 << (tag & 0x3f);
            self.retired_count += 1;
        }
    }

    fn reclaim_retired(&mut self, tag: u8) -> bool {
        if !self.is_retired(tag) {
            return false;
        }
        self.retired[(tag >> 6) as usize] &= !(1u64 << (tag & 0x3f));
        self.retired_count -= 1;
        true
    }

    fn tag_in_flight(&self, tag: u8) -> bool {
        self.slots.iter().any(|s| s.used && s.tag == tag)
    }

    pub(crate) fn alloc(&mut self) -> Result<(usize, u8)> {
        let idx = self.slots.iter().position(|s| !s.used).ok_or(EBUSY)?;

        for _ in 0..POOL_LEN {
            let tag = self.next_tag;
            self.next_tag = if tag >= proto::TAG_POOL_LAST {
                proto::TAG_POOL_FIRST
            } else {
                tag + 1
            };
            if !self.tag_in_flight(tag) && !self.is_retired(tag) {
                self.slots[idx] = Slot {
                    used: true,
                    tag,
                    reply: None,
                };
                return Ok((idx, tag));
            }
        }
        Err(EBUSY)
    }

    pub(crate) fn take_reply(&mut self, idx: usize) -> Option<proto::ControlReply> {
        self.slots[idx].reply.take()
    }

    pub(crate) fn release(&mut self, idx: usize) {
        self.slots[idx] = Slot::FREE;
    }

    // Retire the tag so a late reply cannot be mistaken for a later request's answer.
    pub(crate) fn abandon(&mut self, idx: usize) -> u8 {
        let tag = self.slots[idx].tag;
        self.retire(tag);
        self.slots[idx] = Slot::FREE;
        tag
    }

    pub(crate) fn retired_count(&self) -> u32 {
        self.retired_count
    }

    pub(crate) fn deliver(&mut self, reply: proto::ControlReply) -> Delivery {
        // The reserved entropy tag is claimed first, outstanding request or not.
        if reply.tag == proto::TAG_ENTROPY {
            self.entropy.value = Some(reply.data_lo);
            return Delivery::Entropy;
        }

        if let Some(slot) = self
            .slots
            .iter_mut()
            .find(|s| s.used && s.tag == reply.tag && s.reply.is_none())
        {
            slot.reply = Some(reply);
            return Delivery::Matched;
        }

        if self.reclaim_retired(reply.tag) {
            return Delivery::Unmatched;
        }

        // No match: drop it, never hand it to another waiter.
        Delivery::Unmatched
    }

    pub(crate) fn entropy_begin(&mut self) -> Result<()> {
        if self.entropy.busy {
            return Err(EBUSY);
        }
        self.entropy.busy = true;
        self.entropy.value = None;
        Ok(())
    }

    pub(crate) fn entropy_take(&mut self) -> Option<u32> {
        self.entropy.value.take()
    }

    pub(crate) fn entropy_end(&mut self) {
        self.entropy.busy = false;
    }
}
