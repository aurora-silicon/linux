// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's xART control endpoint (`xars`).
//!
//! `xarm` (endpoint `0x13`) is the enclave-initiated callback channel through
//! which the SEP asks the host to read, write and delete opaque anti-replay
//! records in the shared gigalocker. `xars` (endpoint `0x10`) is the opposite,
//! host-initiated control channel for the same xART service.
//!
//! On macOS 27, `xars` is silent during boot, idle, display sleep and ordinary
//! system sleep; its out-of-line buffers (two 16 KiB pages each way) are
//! registered lazily on first use. During an orderly shutdown, halt or reboot
//! — before filesystems unmount and before the later control-endpoint shutdown
//! handshake — macOS sends two serialized lifecycle requests while `xarm` and
//! the host store remain live:
//!
//! ```text
//! TX 0x0000_0000_000d_0610   endpoint 0x10, tag, op 0x0d (notify storage unmount), 0 OOL bytes
//!    (enclave disables clients via silent xarm 0x1d..=0x1f notifications and drains dirty
//!     root/session records through xarm writes, which the host fsyncs and acknowledges)
//! RX 0x0000_0000_0000_0610   endpoint 0x10, same tag, status 0, 0 OOL bytes, args [0, 0, 0]
//!
//! TX 0x0000_0000_010c_0610   endpoint 0x10, tag, op 0x0c (flush cached state), 1 OOL byte [0x00]
//!    (any final enclave-initiated xarm writes are serviced and fsynced before the reply)
//! RX 0x0000_0000_0000_0610   endpoint 0x10, same tag, status 0, 0 OOL bytes, args [0, 0, 0]
//! ```
//!
//! Only after both operations succeed does the host close its xART store.
//!
//! Linux implements this exact shutdown barrier from a blocking reboot
//! notifier (`SYS_RESTART`, `SYS_HALT`, `SYS_POWER_OFF`), before
//! `device_shutdown()`, while the mailbox, DART and NVMe gigalocker handle are
//! still live. Nothing is sent on suspend, hibernation, panic or emergency
//! restart, and nothing is sent when `xars` is absent, advertises an unknown
//! buffer layout, or the xART store is read-only.
//!
//! The command set is intentionally closed to [`ShutdownOp`]: there is no raw
//! selector, no userspace interface, no forced flush, and no destructive
//! operation (such as session-seed erasure, which macOS sends only when
//! deleting an APFS volume group). Any timeout, refusal or malformed reply
//! after a request is submitted is treated as indeterminate: the driver never
//! retries blindly, never sends flush after an indeterminate unmount, keeps
//! `xarm` servicing alive until ordinary teardown, logs the failure and allows
//! shutdown to proceed within a bounded budget.

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_XARS: u8 = 0x10;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`).
pub(crate) const XARS_NAME: u32 = u32::from_be_bytes(*b"xars");

/// The buffer layout the endpoint advertises in its discovery configuration
/// word: two 16 KiB enclave pages each way. The driver talks to the endpoint
/// only when it advertises this layout.
pub(crate) const XARS_LAYOUT: u32 = 0x0202_0202;

/// Each out-of-line buffer: two 16 KiB enclave pages, as the layout says.
pub(crate) const OOL_SIZE_XARS: usize = 0x8000;

/// Lifecycle operations used in the orderly shutdown barrier.
pub(crate) const OP_FLUSH_CACHED_STATE: u8 = 0x0c;
pub(crate) const OP_NOTIFY_STORAGE_UNMOUNT: u8 = 0x0d;

pub(crate) const STATUS_OK: u8 = 0x00;

/// First transaction tag handed out by [`Exchange`]; monotonic across requests
/// within a boot so a late reply to a timed-out call cannot match the next.
pub(crate) const FIRST_TAG: u8 = 0x06;
const TAG_MIN: u8 = 0x01;
const TAG_MAX: u8 = 0xfe;

/// Per-operation reply bound and total reboot-notifier barrier budget.
pub(crate) const OP_TIMEOUT_MS: u32 = 5_000;
pub(crate) const BARRIER_BUDGET_MS: u32 = 10_000;

/// Non-forced flush payload: a single Boolean `false`. Forced flush (`true`) is
/// reserved for destructive erase flows and is never transmitted here.
const FLUSH_NON_FORCED: [u8; 1] = [0x00];

/// Closed set of host-initiated `xars` operations this driver can transmit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ShutdownOp {
    /// Tell the enclave that xART storage is about to unmount.
    NotifyStorageUnmount,
    /// Ask the enclave to flush any remaining cached state (`force_flush = false`).
    FlushCachedState,
}

impl ShutdownOp {
    pub(crate) const fn opcode(self) -> u8 {
        match self {
            ShutdownOp::NotifyStorageUnmount => OP_NOTIFY_STORAGE_UNMOUNT,
            ShutdownOp::FlushCachedState => OP_FLUSH_CACHED_STATE,
        }
    }

    pub(crate) const fn body(self) -> &'static [u8] {
        match self {
            ShutdownOp::NotifyStorageUnmount => &[],
            ShutdownOp::FlushCachedState => &FLUSH_NON_FORCED,
        }
    }

    pub(crate) const fn ool_length(self) -> u16 {
        self.body().len() as u16
    }

    pub(crate) const fn name(self) -> &'static CStr {
        match self {
            ShutdownOp::NotifyStorageUnmount => c"notify_storage_unmount",
            ShutdownOp::FlushCachedState => c"flush_cached_state",
        }
    }
}

/// Encodes one typed `xars` request in the shared 8-byte xART carrier format.
pub(crate) const fn encode_request(tag: u8, op: ShutdownOp) -> Message {
    let len = op.ool_length().to_le_bytes();
    Message {
        msg0: u64::from_le_bytes([
            EP_XARS,
            tag,
            op.opcode(),
            len[0],
            len[1],
            0,
            0,
            0,
        ]),
        msg1: 0,
    }
}

// Match the mailbox words recorded on a J616s under macOS 27 shutdown.
static_assert!(
    encode_request(FIRST_TAG, ShutdownOp::NotifyStorageUnmount).msg0 == 0x0000_0000_000d_0610
);
static_assert!(
    encode_request(FIRST_TAG, ShutdownOp::FlushCachedState).msg0 == 0x0000_0000_010c_0610
);

/// Decoded 8-byte `xars` reply word.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Reply {
    pub(crate) tag: u8,
    pub(crate) status: u8,
    pub(crate) length: u16,
    pub(crate) args: [u8; 3],
}

/// Decodes a mailbox message if it belongs to [`EP_XARS`].
pub(crate) fn decode_reply(msg: &Message) -> Option<Reply> {
    let b = msg.msg0.to_le_bytes();
    if b[0] != EP_XARS {
        return None;
    }
    Some(Reply {
        tag: b[1],
        status: b[2],
        length: u16::from_le_bytes([b[3], b[4]]),
        args: [b[5], b[6], b[7]],
    })
}

/// Result of one `xars` shutdown operation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// Tag matched, status 0, zero returned length and zero result arguments.
    Accepted,
    /// Tag matched, enclave returned a nonzero `service_status`.
    Refused(u8),
    /// Tag matched, but returned length or result arguments were unexpected.
    Malformed,
    /// Reply carried a different transaction tag (not an answer to this call).
    WrongTag,
    /// No matching reply arrived within the operation deadline.
    NoReply,
}

/// Validates a decoded reply against `expected_tag` for a zero-body response.
pub(crate) fn check_reply(expected_tag: u8, reply: &Reply) -> Outcome {
    if reply.tag != expected_tag {
        return Outcome::WrongTag;
    }
    if reply.status != STATUS_OK {
        return Outcome::Refused(reply.status);
    }
    if reply.length != 0 || reply.args != [0, 0, 0] {
        return Outcome::Malformed;
    }
    Outcome::Accepted
}

/// Lifecycle state of the `xars` shutdown barrier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BarrierState {
    /// Normal operation; barrier has not run yet.
    Idle = 0,
    /// Barrier is in progress; new host mutations are refused while `xarm` stays live.
    Closing = 1,
    /// Unmount and non-forced flush succeeded and the host xART store was closed.
    Completed = 2,
    /// An operation failed, timed out or returned a malformed reply.
    Indeterminate = 3,
    /// Endpoint absent/unknown layout, attach incomplete, or xART read-only.
    Unavailable = 4,
}

impl BarrierState {
    pub(crate) const fn from_u32(v: u32) -> BarrierState {
        match v {
            0 => BarrierState::Idle,
            1 => BarrierState::Closing,
            2 => BarrierState::Completed,
            3 => BarrierState::Indeterminate,
            _ => BarrierState::Unavailable,
        }
    }
}

/// Serialised single-in-flight transaction tracker for `xars`.
pub(crate) struct Exchange {
    waiting: bool,
    tag: u8,
    next_tag: u8,
    generation: u64,
    outcome: Option<Outcome>,
}

impl Exchange {
    pub(crate) const fn new() -> Exchange {
        Exchange {
            waiting: false,
            tag: 0,
            next_tag: FIRST_TAG,
            generation: 0,
            outcome: None,
        }
    }

    fn alloc_tag(&mut self) -> u8 {
        let tag = self.next_tag;
        self.next_tag = if tag >= TAG_MAX { TAG_MIN } else { tag + 1 };
        tag
    }

    /// Starts a new exchange with a fresh non-zero transaction tag.
    pub(crate) fn begin(&mut self) -> (u64, u8) {
        let tag = self.alloc_tag();
        self.generation = self.generation.wrapping_add(1);
        self.waiting = true;
        self.tag = tag;
        self.outcome = None;
        (self.generation, tag)
    }

    /// Returns `(generation, tag)` if an exchange is waiting for its reply.
    #[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
    #[allow(dead_code)]
    pub(crate) fn current(&self) -> Option<(u64, u8)> {
        if self.waiting {
            Some((self.generation, self.tag))
        } else {
            None
        }
    }

    /// Delivers a reply to the active exchange. Returns `true` if the reply
    /// matched the waiting generation and tag.
    pub(crate) fn deliver(&mut self, reply: &Reply) -> bool {
        if !self.waiting || self.outcome.is_some() {
            return false;
        }
        let outcome = check_reply(self.tag, reply);
        if outcome == Outcome::WrongTag {
            return false;
        }
        self.outcome = Some(outcome);
        true
    }

    pub(crate) fn take(&mut self) -> Option<Outcome> {
        let outcome = self.outcome.take();
        if outcome.is_some() {
            self.waiting = false;
        }
        outcome
    }

    pub(crate) fn end(&mut self) {
        self.waiting = false;
        self.outcome = None;
    }
}

/// Pure transport boundary for [`execute_barrier`], allowing KUnit tests to
/// exercise every shutdown-barrier transition and fault path without hardware.
pub(crate) trait BarrierTransport {
    fn now_ms(&self) -> u64;
    fn preconditions_met(&self) -> bool;
    fn begin_closing(&mut self);
    fn register_ool(&mut self) -> Result<()>;
    fn transact(&mut self, op: ShutdownOp, timeout_ms: u32) -> Result<Outcome>;
    fn close_store(&mut self);
}

/// Final disposition returned by [`execute_barrier`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BarrierResult {
    /// Already ran earlier in this boot; nothing sent.
    AlreadyRan(BarrierState),
    /// Preconditions not met; nothing sent.
    Unavailable,
    /// Unmount + non-forced flush succeeded and the host store was closed.
    Completed,
    /// OOL registration failed before any `xars` request was submitted.
    OolFailed,
    /// A submitted `xars` request did not succeed cleanly; store left open.
    Indeterminate {
        op: ShutdownOp,
        outcome: Outcome,
    },
    /// Submitting a `xars` request failed at the transport layer; store left open.
    SendFailed {
        op: ShutdownOp,
    },
    /// Total barrier budget expired before flush could be sent; store left open.
    BudgetExhausted,
}

/// Runs the orderly shutdown barrier once: `Idle -> Closing -> Completed`, or
/// `Unavailable` / `Indeterminate` on precondition or protocol failure.
///
/// Guarantees:
/// - Runs at most once per boot (`state != Idle` is a no-op).
/// - Never sends `FlushCachedState` unless `NotifyStorageUnmount` returned
///   [`Outcome::Accepted`].
/// - Never retries an indeterminate operation.
/// - Closes the host xART store only after both operations succeed.
pub(crate) fn execute_barrier(
    state: &mut BarrierState,
    transport: &mut impl BarrierTransport,
) -> BarrierResult {
    if *state != BarrierState::Idle {
        return BarrierResult::AlreadyRan(*state);
    }
    if !transport.preconditions_met() {
        *state = BarrierState::Unavailable;
        return BarrierResult::Unavailable;
    }

    *state = BarrierState::Closing;
    transport.begin_closing();

    if transport.register_ool().is_err() {
        *state = BarrierState::Indeterminate;
        return BarrierResult::OolFailed;
    }

    let deadline = transport.now_ms().saturating_add(u64::from(BARRIER_BUDGET_MS));

    let unmount_timeout = remaining_op_ms(transport.now_ms(), deadline);
    if unmount_timeout == 0 {
        *state = BarrierState::Indeterminate;
        return BarrierResult::BudgetExhausted;
    }
    match transport.transact(ShutdownOp::NotifyStorageUnmount, unmount_timeout) {
        Ok(Outcome::Accepted) => {}
        Ok(outcome) => {
            *state = BarrierState::Indeterminate;
            return BarrierResult::Indeterminate {
                op: ShutdownOp::NotifyStorageUnmount,
                outcome,
            };
        }
        Err(_) => {
            *state = BarrierState::Indeterminate;
            return BarrierResult::SendFailed {
                op: ShutdownOp::NotifyStorageUnmount,
            };
        }
    }

    let flush_timeout = remaining_op_ms(transport.now_ms(), deadline);
    if flush_timeout == 0 {
        *state = BarrierState::Indeterminate;
        return BarrierResult::BudgetExhausted;
    }
    match transport.transact(ShutdownOp::FlushCachedState, flush_timeout) {
        Ok(Outcome::Accepted) => {}
        Ok(outcome) => {
            *state = BarrierState::Indeterminate;
            return BarrierResult::Indeterminate {
                op: ShutdownOp::FlushCachedState,
                outcome,
            };
        }
        Err(_) => {
            *state = BarrierState::Indeterminate;
            return BarrierResult::SendFailed {
                op: ShutdownOp::FlushCachedState,
            };
        }
    }

    transport.close_store();
    *state = BarrierState::Completed;
    BarrierResult::Completed
}

fn remaining_op_ms(now_ms: u64, deadline_ms: u64) -> u32 {
    let left = deadline_ms.saturating_sub(now_ms);
    u32::try_from(left.min(u64::from(OP_TIMEOUT_MS))).unwrap_or(OP_TIMEOUT_MS)
}

struct LiveBarrier<'a>(&'a SepData);

impl BarrierTransport for LiveBarrier<'_> {
    fn now_ms(&self) -> u64 {
        boottime_ms()
    }

    fn preconditions_met(&self) -> bool {
        !self.0.shutting_down.load(Relaxed)
            && self.0.phase.load(Relaxed) == PHASE_READY
            && self.0.sleep_gate() == pm::SleepGate::Open
            && self.0.xars_offered()
            && self.0.xart_writable()
    }

    fn begin_closing(&mut self) {
        self.0.xars_barrier_state.store(BarrierState::Closing as u32, Release);
        // Stop new entropy draws and Touch ID captures while keeping the
        // mailbox and xarm callback path fully active for the enclave's drain.
        self.0.rng_sleep_prepare();
        self.0.sleep_prepare();
    }

    fn register_ool(&mut self) -> Result<()> {
        self.0.register_ool(&self.0.ool_xars)
    }

    fn transact(&mut self, op: ShutdownOp, timeout_ms: u32) -> Result<Outcome> {
        self.0.xars_transact(op, timeout_ms)
    }

    fn close_store(&mut self) {
        // Acquiring `store` also serialises behind any trailing xarm callback
        // in `rx_work`, which fsyncs every accepted write before replying.
        let _ = self.0.store.lock().take();
    }
}

impl SepData {
    /// Whether the enclave offers `xars` with the 2-page layout the driver knows.
    pub(crate) fn xars_offered(&self) -> bool {
        self.endpoint_present(EP_XARS)
            && self.xars_named.load(Relaxed)
            && self.xars_layout.load(Relaxed) == XARS_LAYOUT
    }

    /// Records the discovery messages of `xars`.
    pub(crate) fn xars_discovered(&self, f: &Fields) {
        if f.param != EP_XARS {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.xars_named.store(f.data_lo == XARS_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.xars_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    /// Updates passive diagnostics for `xars` once bring-up finishes or after
    /// `complete_attach` changes xART writability. Sends nothing and does not
    /// register OOL buffers.
    pub(crate) fn xars_attach(&self) {
        if !self.xars_offered() {
            if self.endpoint_present(EP_XARS) {
                dev_dbg!(
                    self.dev,
                    "xars: the endpoint advertises layout {:#010x}; left alone\n",
                    self.xars_layout.load(Relaxed)
                );
            }
            diag::xars(diag::Xars::Absent);
        } else if self.xart_writable() {
            diag::xars(diag::Xars::Ready);
        } else {
            diag::xars(diag::Xars::ReadOnly);
        }
    }

    /// Whether the shutdown barrier is currently closing (used to count
    /// shutdown-triggered `xarm` writes).
    pub(crate) fn xars_is_closing(&self) -> bool {
        BarrierState::from_u32(self.xars_barrier_state.load(Acquire)) == BarrierState::Closing
    }

    /// Runs the orderly shutdown/reboot barrier (`notify_storage_unmount` then
    /// non-forced `flush_cached_state`, closing the host xART store only after
    /// both succeed). Called from the blocking reboot notifier before
    /// `device_shutdown()`.
    pub(crate) fn xars_shutdown_barrier(&self) {
        let mut state_guard = self.xars_barrier_lock.lock();
        let start_ms = boottime_ms();
        self.xars_shutdown_writes.store(0, Relaxed);

        let mut transport = LiveBarrier(self);
        let result = execute_barrier(&mut *state_guard, &mut transport);
        self.xars_barrier_state.store(*state_guard as u32, Release);
        let elapsed_ms = boottime_ms().saturating_sub(start_ms);
        let writes = self.xars_shutdown_writes.load(Relaxed);

        match result {
            BarrierResult::AlreadyRan(_) => {}
            BarrierResult::Unavailable => {
                dev_dbg!(
                    self.dev,
                    "xars: shutdown barrier unavailable (phase {}, offered {}, writable {})\n",
                    self.phase.load(Relaxed),
                    self.xars_offered(),
                    self.xart_writable()
                );
            }
            BarrierResult::Completed => {
                diag::xars(diag::Xars::Flushed);
                dev_info_ratelimited!(
                    self.dev,
                    "xars: shutdown barrier complete (unmount + flush, {} xarm writes in {} ms)\n",
                    writes,
                    elapsed_ms
                );
            }
            BarrierResult::OolFailed => {
                diag::xars(diag::Xars::Failed);
                dev_warn_ratelimited!(
                    self.dev,
                    "xars: shutdown barrier skipped: OOL buffer registration failed ({} ms)\n",
                    elapsed_ms
                );
            }
            BarrierResult::Indeterminate { op, outcome } => {
                diag::xars(diag::Xars::Failed);
                dev_warn_ratelimited!(
                    self.dev,
                    "xars: shutdown barrier indeterminate at {} ({:?}, {} xarm writes in {} ms)\n",
                    op.name(),
                    outcome,
                    writes,
                    elapsed_ms
                );
            }
            BarrierResult::SendFailed { op } => {
                diag::xars(diag::Xars::Failed);
                dev_warn_ratelimited!(
                    self.dev,
                    "xars: shutdown barrier could not send {} ({} xarm writes in {} ms)\n",
                    op.name(),
                    writes,
                    elapsed_ms
                );
            }
            BarrierResult::BudgetExhausted => {
                diag::xars(diag::Xars::Failed);
                dev_warn_ratelimited!(
                    self.dev,
                    "xars: shutdown barrier budget exhausted ({} xarm writes in {} ms)\n",
                    writes,
                    elapsed_ms
                );
            }
        }
    }

    fn xars_transact(&self, op: ShutdownOp, timeout_ms: u32) -> Result<Outcome> {
        let (_generation, tag) = self.xars_rx.lock().begin();
        let body = op.body();
        let sent = if !body.is_empty() {
            self.ool_write(&self.ool_xars, 0, body)
                .and_then(|()| self.send(encode_request(tag, op)))
        } else {
            self.send(encode_request(tag, op))
        };
        if let Err(e) = sent {
            self.xars_rx.lock().end();
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(timeout_ms);
        let mut guard = self.xars_rx.lock();
        loop {
            if self.shutting_down.load(Relaxed) {
                guard.end();
                return Err(ENODEV);
            }
            if let Some(outcome) = guard.take() {
                return Ok(outcome);
            }
            if remaining == 0 {
                guard.end();
                return Ok(Outcome::NoReply);
            }
            // Uninterruptible wait: reboot path must not spin on pending signals.
            match self.xars_wq.wait_timeout(&mut guard, remaining) {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { .. } => remaining = 0,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    pub(crate) fn on_xars(&self, msg: Message) {
        let Some(reply) = decode_reply(&msg) else {
            self.xars_unexpected(&msg);
            return;
        };
        let delivered = self.xars_rx.lock().deliver(&reply);
        if delivered {
            self.xars_wq.notify_all();
        } else {
            self.xars_unexpected(&msg);
        }
    }

    fn xars_unexpected(&self, msg: &Message) {
        if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "xars: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_xars)]
mod tests {
    use super::*;

    // Discovery words from our Linux boot logs (M1 Pro, M2, M4 Pro, A18 Pro).
    const DISCOVER_NAME: u64 = 0x7861_7273_1000_00fd;
    const DISCOVER_LAYOUT: u64 = 0x0202_0202_1001_00fd;

    // Shutdown mailbox words recorded on a J616s under macOS 27.0
    // (unmount notice, then non-forced cached-state flush).
    const TRACE_TX_UNMOUNT: u64 = 0x0000_0000_000d_0610;
    const TRACE_TX_FLUSH: u64 = 0x0000_0000_010c_0610;
    // Reply word recorded on J473 under macOS 27.2 (tag 0x06, status 0, len 0).
    const TRACE_RX_OK: u64 = 0x0000_0000_0000_0610;

    #[test]
    fn discovery_words_match_expected_name_and_layout() {
        let name = decode(&Message {
            msg0: DISCOVER_NAME,
            msg1: 0,
        });
        assert_eq!(name.ep, EP_DISCOVER);
        assert_eq!(name.param, EP_XARS);
        assert_eq!(name.ty, DISCOVER_TYPE_DESCRIPTOR);
        assert_eq!(name.data_lo, XARS_NAME);

        let cfg = decode(&Message {
            msg0: DISCOVER_LAYOUT,
            msg1: 0,
        });
        assert_eq!(cfg.ep, EP_DISCOVER);
        assert_eq!(cfg.param, EP_XARS);
        assert_eq!(cfg.ty, DISCOVER_TYPE_CONFIG);
        assert_eq!(cfg.data_lo, XARS_LAYOUT);
    }

    #[test]
    fn shutdown_requests_match_the_recordings() {
        let unmount = encode_request(FIRST_TAG, ShutdownOp::NotifyStorageUnmount);
        assert_eq!(unmount.msg0, TRACE_TX_UNMOUNT);
        assert_eq!(unmount.msg1, 0);
        assert_eq!(ShutdownOp::NotifyStorageUnmount.body(), &[]);

        let flush = encode_request(FIRST_TAG, ShutdownOp::FlushCachedState);
        assert_eq!(flush.msg0, TRACE_TX_FLUSH);
        assert_eq!(flush.msg1, 0);
        // Non-forced flush only: force_flush byte must always be 0.
        assert_eq!(ShutdownOp::FlushCachedState.body(), &[0x00]);
    }

    #[test]
    fn recorded_reply_validates_and_bad_replies_fail() {
        let reply = decode_reply(&Message {
            msg0: TRACE_RX_OK,
            msg1: 0,
        })
        .unwrap();
        assert_eq!(reply.tag, FIRST_TAG);
        assert_eq!(reply.status, STATUS_OK);
        assert_eq!(reply.length, 0);
        assert_eq!(reply.args, [0, 0, 0]);
        assert_eq!(check_reply(FIRST_TAG, &reply), Outcome::Accepted);

        // Wrong endpoint is rejected by decode_reply.
        assert!(decode_reply(&Message {
            msg0: 0x0000_0000_0000_0613,
            msg1: 0,
        })
        .is_none());

        // Wrong tag.
        assert_eq!(check_reply(FIRST_TAG + 1, &reply), Outcome::WrongTag);

        // Nonzero service status is a refusal.
        let refused = Reply {
            status: 0x16,
            ..reply
        };
        assert_eq!(check_reply(FIRST_TAG, &refused), Outcome::Refused(0x16));

        // Unexpected returned length or result argument is malformed.
        let with_len = Reply { length: 1, ..reply };
        assert_eq!(check_reply(FIRST_TAG, &with_len), Outcome::Malformed);
        let with_arg = Reply {
            args: [1, 0, 0],
            ..reply
        };
        assert_eq!(check_reply(FIRST_TAG, &with_arg), Outcome::Malformed);
    }

    #[test]
    fn exchange_allocates_fresh_tags_and_ignores_late_replies() {
        let mut ex = Exchange::new();
        assert_eq!(ex.current(), None);

        let (g1, t1) = ex.begin();
        assert_eq!(t1, FIRST_TAG);
        assert_eq!(ex.current(), Some((g1, t1)));

        // First call times out; second call gets a fresh tag.
        ex.end();
        let (g2, t2) = ex.begin();
        assert_ne!(t2, t1);
        assert_ne!(t2, 0);
        assert_eq!(ex.current(), Some((g2, t2)));

        // Late reply for t1 must not satisfy t2.
        let late = Reply {
            tag: t1,
            status: STATUS_OK,
            length: 0,
            args: [0; 3],
        };
        assert!(!ex.deliver(&late));
        assert_eq!(ex.take(), None);

        // Matching reply for t2 succeeds; a duplicate is ignored.
        let ok = Reply { tag: t2, ..late };
        assert!(ex.deliver(&ok));
        assert!(!ex.deliver(&Reply { status: 2, ..ok }));
        assert_eq!(ex.take(), Some(Outcome::Accepted));
        assert_eq!(ex.current(), None);
    }

    #[test]
    fn tag_pool_wraps_without_zero() {
        let mut ex = Exchange::new();
        for _ in 0..300 {
            let (_, tag) = ex.begin();
            assert!(tag >= TAG_MIN && tag <= TAG_MAX);
            ex.end();
        }
    }

    struct MockTransport {
        now: u64,
        preconditions: bool,
        ool_ok: bool,
        unmount_step_ms: u64,
        flush_step_ms: u64,
        unmount_res: Result<Outcome>,
        flush_res: Result<Outcome>,
        closing_called: bool,
        ool_called: bool,
        store_closed: bool,
        ops: [Option<ShutdownOp>; 4],
        op_count: usize,
        timeouts: [u32; 4],
    }

    impl MockTransport {
        fn healthy() -> Self {
            MockTransport {
                now: 1000,
                preconditions: true,
                ool_ok: true,
                unmount_step_ms: 400,
                flush_step_ms: 50,
                unmount_res: Ok(Outcome::Accepted),
                flush_res: Ok(Outcome::Accepted),
                closing_called: false,
                ool_called: false,
                store_closed: false,
                ops: [None; 4],
                op_count: 0,
                timeouts: [0; 4],
            }
        }
    }

    impl BarrierTransport for MockTransport {
        fn now_ms(&self) -> u64 {
            self.now
        }

        fn preconditions_met(&self) -> bool {
            self.preconditions
        }

        fn begin_closing(&mut self) {
            self.closing_called = true;
        }

        fn register_ool(&mut self) -> Result<()> {
            self.ool_called = true;
            if self.ool_ok {
                Ok(())
            } else {
                Err(ETIMEDOUT)
            }
        }

        fn transact(&mut self, op: ShutdownOp, timeout_ms: u32) -> Result<Outcome> {
            let idx = self.op_count;
            self.ops[idx] = Some(op);
            self.timeouts[idx] = timeout_ms;
            self.op_count += 1;
            match op {
                ShutdownOp::NotifyStorageUnmount => {
                    self.now = self.now.saturating_add(self.unmount_step_ms);
                    self.unmount_res
                }
                ShutdownOp::FlushCachedState => {
                    self.now = self.now.saturating_add(self.flush_step_ms);
                    self.flush_res
                }
            }
        }

        fn close_store(&mut self) {
            self.store_closed = true;
        }
    }

    #[test]
    fn happy_path_runs_unmount_then_flush_then_closes_store_once() {
        let mut state = BarrierState::Idle;
        let mut t = MockTransport::healthy();

        assert_eq!(execute_barrier(&mut state, &mut t), BarrierResult::Completed);
        assert_eq!(state, BarrierState::Completed);
        assert!(t.closing_called);
        assert!(t.ool_called);
        assert!(t.store_closed);
        assert_eq!(t.op_count, 2);
        assert_eq!(t.ops[0], Some(ShutdownOp::NotifyStorageUnmount));
        assert_eq!(t.ops[1], Some(ShutdownOp::FlushCachedState));
        assert_eq!(t.timeouts[0], OP_TIMEOUT_MS);
        assert_eq!(t.timeouts[1], OP_TIMEOUT_MS);

        // A second invocation (e.g. from device shutdown) sends nothing.
        assert_eq!(
            execute_barrier(&mut state, &mut t),
            BarrierResult::AlreadyRan(BarrierState::Completed)
        );
        assert_eq!(t.op_count, 2);
    }

    #[test]
    fn unavailable_preconditions_send_nothing() {
        let mut state = BarrierState::Idle;
        let mut t = MockTransport::healthy();
        t.preconditions = false;

        assert_eq!(
            execute_barrier(&mut state, &mut t),
            BarrierResult::Unavailable
        );
        assert_eq!(state, BarrierState::Unavailable);
        assert!(!t.closing_called);
        assert!(!t.ool_called);
        assert!(!t.store_closed);
        assert_eq!(t.op_count, 0);
    }

    #[test]
    fn ool_failure_leaves_store_open_and_sends_no_xars() {
        let mut state = BarrierState::Idle;
        let mut t = MockTransport::healthy();
        t.ool_ok = false;

        assert_eq!(execute_barrier(&mut state, &mut t), BarrierResult::OolFailed);
        assert_eq!(state, BarrierState::Indeterminate);
        assert!(!t.store_closed);
        assert_eq!(t.op_count, 0);
    }

    #[test]
    fn indeterminate_unmount_never_sends_flush_or_closes_store() {
        for bad in [Outcome::NoReply, Outcome::Refused(0x16), Outcome::Malformed] {
            let mut state = BarrierState::Idle;
            let mut t = MockTransport::healthy();
            t.unmount_res = Ok(bad);

            assert_eq!(
                execute_barrier(&mut state, &mut t),
                BarrierResult::Indeterminate {
                    op: ShutdownOp::NotifyStorageUnmount,
                    outcome: bad,
                }
            );
            assert_eq!(state, BarrierState::Indeterminate);
            assert_eq!(t.op_count, 1);
            assert!(!t.store_closed);
        }
    }

    #[test]
    fn indeterminate_flush_leaves_store_open_and_does_not_retry() {
        for bad in [Outcome::NoReply, Outcome::Refused(0x02), Outcome::Malformed] {
            let mut state = BarrierState::Idle;
            let mut t = MockTransport::healthy();
            t.flush_res = Ok(bad);

            assert_eq!(
                execute_barrier(&mut state, &mut t),
                BarrierResult::Indeterminate {
                    op: ShutdownOp::FlushCachedState,
                    outcome: bad,
                }
            );
            assert_eq!(state, BarrierState::Indeterminate);
            assert_eq!(t.op_count, 2);
            assert!(!t.store_closed);
        }
    }

    #[test]
    fn slow_unmount_caps_or_exhausts_flush_budget() {
        // 8 s unmount leaves 2 s for flush (below OP_TIMEOUT_MS).
        let mut state = BarrierState::Idle;
        let mut t = MockTransport::healthy();
        t.unmount_step_ms = 8_000;
        assert_eq!(execute_barrier(&mut state, &mut t), BarrierResult::Completed);
        assert_eq!(t.timeouts[1], 2_000);

        // 10 s unmount leaves 0 ms: flush is not sent and store stays open.
        let mut state = BarrierState::Idle;
        let mut t = MockTransport::healthy();
        t.unmount_step_ms = u64::from(BARRIER_BUDGET_MS);
        assert_eq!(
            execute_barrier(&mut state, &mut t),
            BarrierResult::BudgetExhausted
        );
        assert_eq!(state, BarrierState::Indeterminate);
        assert_eq!(t.op_count, 1);
        assert!(!t.store_closed);
    }
}
