// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Closed-context kill requests and the command-pin release witness.

use super::{Context, Deferred, Queue};
use core::sync::atomic::{fence, AtomicU32, Ordering};
use kernel::{prelude::*, sync::Arc};

pub(crate) const POLL_NS: u64 = 2_500_000_000;
const QUARANTINED_DELAY_NS: u64 = 250_000_000;
const CANCELLED_DELAY_NS: u64 = 2_000_000_000;
const ACK_SLOTS: usize = 16;

// Firmware-visible cookie namespace persists for the loaded module's lifetime.
static NEXT_COOKIE: AtomicU32 = AtomicU32::new(1);

pub(crate) fn next_cookie() -> u64 {
    loop {
        let cookie = NEXT_COOKIE.fetch_add(1, Ordering::Relaxed);
        if cookie != 0 {
            return cookie.into();
        }
    }
}

pub(crate) struct Acks([u64; ACK_SLOTS]);
impl Acks {
    pub(crate) fn new() -> Self {
        Self([0; ACK_SLOTS])
    }
    pub(crate) fn note(&mut self, cookie: u64) {
        if cookie == 0 || self.contains(cookie) {
            return;
        }
        if let Some(slot) = self.0.iter_mut().find(|slot| **slot == 0) {
            *slot = cookie;
        }
    }
    pub(crate) fn contains(&self, cookie: u64) -> bool {
        cookie != 0 && self.0.contains(&cookie)
    }
    pub(crate) fn forget(&mut self, cookie: u64) {
        for slot in &mut self.0 {
            if *slot == cookie {
                *slot = 0;
            }
        }
    }
    pub(crate) fn snapshot(&self) -> [u64; ACK_SLOTS] {
        self.0
    }
}

pub(crate) struct State {
    pub(crate) slot: u8,
    pub(crate) qids: u128,
    pub(crate) killed: u32,
}
impl State {
    pub(crate) fn read(context: &Context) -> Result<Self> {
        let words = context.scheduler_owner().read_words::<14>(0)?;
        let mut bytes = [0u8; 0x38];
        for (destination, word) in bytes.chunks_exact_mut(4).zip(words) {
            destination.copy_from_slice(&word.to_le_bytes());
        }
        fence(Ordering::Acquire);
        Ok(Self::decode(&bytes))
    }
    fn decode(bytes: &[u8; 0x38]) -> Self {
        let mut low = [0; 8];
        let mut high = [0; 8];
        let mut killed = [0; 4];
        low.copy_from_slice(&bytes[0x12..0x1a]);
        high.copy_from_slice(&bytes[0x1a..0x22]);
        killed.copy_from_slice(&bytes[0x34..0x38]);
        Self {
            slot: bytes[1],
            qids: u128::from(u64::from_le_bytes(low))
                | (u128::from(u64::from_le_bytes(high)) << 64),
            killed: u32::from_le_bytes(killed),
        }
    }
    pub(crate) fn scope(&self, target: u8, own: u128, foreign: u128) -> Option<u128> {
        if self.slot == 0xff || target >= 128 || self.qids & (1u128 << target) == 0 {
            return None;
        }
        let allowed = (own | (1u128 << target)) & !foreign & !0b11;
        (self.qids & !allowed == 0).then_some(self.qids)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Published,
    Acknowledged,
    Unproven,
    Released,
}

pub(super) struct Transaction {
    cookie: u64,
    published: u64,
    phase: Phase,
}

impl Queue {
    pub(crate) fn exit_candidate(&self, context: &Arc<Context>, age: u64) -> bool {
        Arc::ptr_eq(self.context(), context)
            && self.released
            && self.active.len != 0
            && self.exit.is_none()
            && (self.quarantined || self.retired_by_teardown)
            && age
                >= if self.quarantined {
                    QUARANTINED_DELAY_NS
                } else {
                    CANCELLED_DELAY_NS
                }
    }
    pub(crate) fn exit_owner(&self, context: &Arc<Context>) -> Option<u64> {
        if !self
            .active
            .iter()
            .all(|active| Arc::ptr_eq(&active.packet.context, context))
        {
            return None;
        }
        self.owner.or_else(|| {
            self.active
                .back()
                .and_then(|active| active.ticket.map(|ticket| ticket.owner))
        })
    }
    pub(crate) fn record_exit(&mut self, cookie: u64, now: u64) {
        self.exit = Some(Transaction {
            cookie,
            published: now,
            phase: Phase::Published,
        });
    }
    pub(crate) fn exit_polling(&self, now: u64) -> bool {
        self.exit.as_ref().is_some_and(|exit| {
            exit.phase == Phase::Published && now.saturating_sub(exit.published) < POLL_NS
        })
    }
    pub(crate) fn exit_cookie(&self) -> Option<u64> {
        self.exit
            .as_ref()
            .filter(|exit| matches!(exit.phase, Phase::Published | Phase::Acknowledged))
            .map(|exit| exit.cookie)
    }
    pub(crate) fn settle_exit(
        &mut self,
        acks: &Acks,
        recovery_closed: bool,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        let Some(cookie) = self.exit_cookie() else {
            return Ok(());
        };
        if !acks.contains(cookie) {
            return Ok(());
        }
        self.exit.as_mut().ok_or(EIO)?.phase = Phase::Acknowledged;
        if self.active.len != 0 {
            if !recovery_closed {
                return Ok(());
            }
            let state = match State::read(self.context()) {
                Ok(state) => state,
                Err(_) => return Ok(()),
            };
            if state.killed != 1 {
                self.exit.as_mut().ok_or(EIO)?.phase = Phase::Unproven;
                return Ok(());
            }
            if !self.active.iter().all(|active| {
                active
                    .packet
                    .completion
                    .compute_words()
                    .is_ok_and(|words| words == [0, 0] || words[1] != 0)
            }) {
                return Ok(());
            }
        }
        while let Some(active) = self.active.front() {
            defer(Deferred::Retired(
                active.packet.completion.clone(),
                Err(if self.spared_deferred {
                    ENODATA
                } else {
                    ECANCELED
                }),
            ))?;
            drop(self.active.pop());
        }
        self.exit.as_mut().ok_or(EIO)?.phase = Phase::Released;
        self.quarantined = true;
        self.awaiting_witness = false;
        self.spared_deferred = false;
        self.spared_quarantine = false;
        self.retire_pending = false;
        self.retirement_ready = false;
        Ok(())
    }
}
