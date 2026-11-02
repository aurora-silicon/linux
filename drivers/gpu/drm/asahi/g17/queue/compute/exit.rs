// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Closed-context kill requests and the command-pin release witness.

use super::{Context, Deferred, Queue};
use core::sync::atomic::{fence, AtomicU32, Ordering};
use kernel::{prelude::*, sync::Arc};

pub(crate) const POLL_NS: u64 = 2_500_000_000;
/// A closed context's quarantined queue is left this long before its kill, so that ordinary
/// retirement goes first.
pub(crate) const QUARANTINED_DELAY_NS: u64 = 250_000_000;
const CANCELLED_DELAY_NS: u64 = 2_000_000_000;
/// The M5 Ultra lane's kill settles only on its failed kicks' exact drain (`Queue::settle_exit_m5`
/// in `queue::m5`): its acknowledgement's window, then the drain's, as long again.
pub(crate) const M5_SETTLE_NS: u64 = 2 * POLL_NS;

// Firmware-visible cookie namespace persists for the loaded module's lifetime.
static NEXT_COOKIE: AtomicU32 = AtomicU32::new(1);

pub(crate) fn next_cookie() -> Result<u64> {
    // Never reuse an identity while a delayed firmware ACK can still exist.
    // The final counter value is an exhaustion marker, not a wrapping cookie.
    NEXT_COOKIE
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| next.checked_add(1))
        .map(u64::from)
        .map_err(|_| EOVERFLOW)
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

/// One published kill of a closed context, from its publication to its settlement: Neo's compute
/// queues and the M5 Ultra's compute lanes (`queue::m5`) each keep their own.
pub(crate) struct Transaction {
    cookie: u64,
    published: u64,
    phase: Phase,
    announced: Option<u64>,
}

impl Transaction {
    /// A kill just published with `cookie`.
    pub(crate) fn new(cookie: u64, now: u64) -> Self {
        Self { cookie, published: now, phase: Phase::Published, announced: None }
    }
    /// The firmware acknowledged the kill and the killed commands were settled.
    pub(crate) fn released(&self) -> bool {
        self.phase == Phase::Released
    }
    /// Acknowledged (or acknowledged and not yet proved by the scheduler page): settlement next.
    pub(crate) fn settling(&self) -> bool {
        matches!(self.phase, Phase::Acknowledged | Phase::Unproven)
    }
    /// When the kill was published.
    pub(crate) fn published_at(&self) -> u64 {
        self.published
    }
    /// The scheduler page did not show the killed state at settlement: re-read on later passes.
    pub(crate) fn mark_unproven(&mut self) {
        self.phase = Phase::Unproven;
    }
    /// Settled.
    pub(crate) fn mark_released(&mut self) {
        self.phase = Phase::Released;
    }
    /// A failed doorbell does not undo the published kill or consume its retry.
    /// The caller retries under the device mutex; only the original record exists.
    pub(crate) fn announce(&mut self, now: u64, notify: impl FnOnce() -> Result) -> Result {
        if self.phase != Phase::Published || self.announced.is_some() {
            return Ok(());
        }
        notify()?;
        self.announced = Some(now);
        Ok(())
    }
    pub(crate) fn polling(&self, now: u64) -> bool {
        self.phase == Phase::Published
            && now.saturating_sub(self.announced.unwrap_or(self.published)) < POLL_NS
    }
    /// Not yet settled, and published less than `window` ago.
    pub(crate) fn open_within(&self, now: u64, window: u64) -> bool {
        self.phase != Phase::Released && now.saturating_sub(self.published) < window
    }
    /// Only this kill's exact nonzero cookie acknowledges it, once; `qid` is traced.
    pub(crate) fn acknowledge(&mut self, cookie: u64, qid: u8) -> bool {
        if cookie == 0 || self.cookie != cookie || self.phase != Phase::Published {
            return false;
        }
        self.phase = Phase::Acknowledged;
        crate::g17::trace::teardown(4, cookie, u64::from(qid), 0);
        true
    }
}

impl Queue {
    /// A running command with ray traversal scratch is never killed. On Neo, killing a
    /// traversal that stopped making progress either crashed the GPU firmware
    /// when the queue's pool was then released, or stalled every later render
    /// when it was retained; left alone, the rest of the GPU kept working. Such
    /// a queue stays quarantined and retained until its commands retire. A
    /// traversal the firmware reported failed no longer runs (see `fault_pending`).
    fn active_traversal(&self) -> bool {
        self.active.iter().any(|active| {
            matches!(
                &active.packet.command,
                crate::g17::command::Validated::Compute { scratch, .. } if scratch.enabled()
            )
        })
    }
    pub(crate) fn exit_candidate(&self, context: &Arc<Context>, age: u64) -> bool {
        if self.graph.m5 {
            return self.exit_candidate_m5(context, age);
        }
        self.context().is_some_and(|bound| Arc::ptr_eq(bound, context))
            && self.released
            && self.active.len != 0
            && self.exit.is_none()
            && (!self.active_traversal() || self.fault_pending())
            && (self.quarantined || self.retired_by_teardown)
            && age
                >= if self.quarantined {
                    QUARANTINED_DELAY_NS
                } else {
                    CANCELLED_DELAY_NS
                }
    }
    /// A kill of `context` was published through this queue.
    pub(crate) fn exit_published(&self, context: &Arc<Context>) -> bool {
        self.exit.is_some() && self.context().is_some_and(|bound| Arc::ptr_eq(bound, context))
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
    /// The kill of this queue's failed context waits; returns how long it has waited, and
    /// whether this is the first time it crossed `slow`.
    pub(crate) fn note_kill_wait(&mut self, now: u64, slow: u64) -> (u64, bool) {
        let (since, warned) = self.kill_wait.get_or_insert((now, false));
        let waited = now.saturating_sub(*since);
        let crossed = waited >= slow && !*warned;
        *warned |= crossed;
        (waited, crossed)
    }
    /// How long the kill of this queue's failed context has been waiting, if it is.
    pub(crate) fn kill_waiting(&self, now: u64) -> Option<u64> {
        self.kill_wait.map(|(since, _)| now.saturating_sub(since))
    }
    /// The kill was published: how long it waited, if it did.
    pub(crate) fn take_kill_wait(&mut self, now: u64) -> u64 {
        self.kill_wait.take().map_or(0, |(since, _)| now.saturating_sub(since))
    }
    pub(crate) fn record_exit(&mut self, cookie: u64, now: u64) {
        self.exit = Some(Transaction::new(cookie, now));
    }
    /// A failed doorbell does not undo the published kill or consume its retry
    /// ([`Transaction::announce`]).
    pub(crate) fn announce_exit(&mut self, now: u64, notify: impl FnOnce() -> Result) -> Result {
        match self.exit.as_mut() {
            Some(exit) => exit.announce(now, notify),
            None => Ok(()),
        }
    }
    pub(crate) fn exit_polling(&self, now: u64) -> bool {
        self.exit.as_ref().is_some_and(|exit| {
            exit.polling(now)
                // The M5 Ultra lane's kill settles on its failed kicks' exact drain, which the
                // event worker polls for within the kill's window.
                || (self.graph.m5 && exit.settling() && exit.open_within(now, M5_SETTLE_NS))
        })
    }
    /// Event consumption and record_exit are serialized by the device mutex.
    /// A kill pins this exact queue transaction through its final witness;
    /// duplicate, unknown and terminal cookies cannot acknowledge another owner.
    pub(crate) fn acknowledge_exit(&mut self, cookie: u64) -> bool {
        let qid = self.qid();
        self.exit.as_mut().is_some_and(|exit| exit.acknowledge(cookie, qid))
    }
    pub(crate) fn settle_exit(
        &mut self,
        recovery_closed: bool,
        complete_qos: &mut impl FnMut(crate::g17::qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        if self.graph.m5 {
            return self.settle_exit_m5(recovery_closed, complete_qos, defer);
        }
        let Some(phase) = self.exit.as_ref().map(|exit| exit.phase) else {
            return Ok(());
        };
        if !matches!(phase, Phase::Acknowledged | Phase::Unproven) {
            return Ok(());
        }
        // An unproven kill (acknowledged before the scheduler page showed the killed
        // state) is re-read on later passes and settles only once the page proves it,
        // even after its commands were retired by other means.
        if self.active.len != 0 || phase == Phase::Unproven {
            if !recovery_closed {
                return Ok(());
            }
            let Some(context) = self.context() else {
                return Ok(());
            };
            let state = match State::read(context) {
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
            if active.qos_pending {
                complete_qos(self.qos_owner())?;
                self.active.iter_mut().next().ok_or(EIO)?.qos_pending = false;
            }
            let active = self.active.front().ok_or(EIO)?;
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
        // The acknowledged kill and command settlement replace the ordinary
        // drain witness, including a failed first publication after rebinding.
        super::Previous::retire_witnessed(&mut self.previous);
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

#[kernel::macros::kunit_tests(asahi_g17_exit_transaction)]
mod transaction_tests {
    use super::{Transaction, M5_SETTLE_NS, POLL_NS};

    #[test]
    fn only_the_exact_cookie_acknowledges_a_kill_once() {
        let mut kill = Transaction::new(7, 1_000);
        assert!(kill.polling(1_000) && !kill.polling(1_000 + POLL_NS) && !kill.settling());
        assert!(!kill.acknowledge(0, 9));
        assert!(!kill.acknowledge(8, 9));
        assert!(kill.acknowledge(7, 9));
        assert!(!kill.acknowledge(7, 9));
        assert!(kill.settling() && !kill.polling(1_000));
        assert!(kill.open_within(1_000 + M5_SETTLE_NS - 1, M5_SETTLE_NS));
        assert!(!kill.open_within(1_000 + M5_SETTLE_NS, M5_SETTLE_NS));
        kill.mark_unproven();
        assert!(kill.settling() && !kill.released());
        kill.mark_released();
        assert!(kill.released() && !kill.settling() && !kill.open_within(1_000, M5_SETTLE_NS));
    }

    #[test]
    fn a_kill_rings_its_doorbell_once() {
        let mut kill = Transaction::new(7, 1_000);
        assert!(kill.announce(1_001, || Err(kernel::error::code::EIO)).is_err());
        let mut rung = 0;
        assert!(kill.announce(1_002, || { rung += 1; Ok(()) }).is_ok());
        assert!(kill.announce(1_003, || { rung += 1; Ok(()) }).is_ok());
        assert_eq!(rung, 1);
        assert!(kill.polling(1_002 + POLL_NS - 1) && !kill.polling(1_002 + POLL_NS));
    }
}
