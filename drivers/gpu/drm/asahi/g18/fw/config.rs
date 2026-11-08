// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Fixed choices for the currently qualified G17P firmware path.
//!
//! Values preserve the former module-parameter defaults. Firmware ABI
//! alternatives require a code change and qualification, not a boot-time mix
//! of independent experimental switches. Debugging and timeout controls remain
//! module parameters in `asahi.rs`.

pub(crate) const RENDER_SLOT_AFFINITY: u32 = 1;
pub(crate) const RENDER_SLOT_GLOBALS: u32 = 3;
pub(crate) const RENDER_NOP_RETIRE: u32 = 1;
pub(crate) const REG_DUMP: u32 = 0;
pub(crate) const SCAN_GATE: u32 = 0;
pub(crate) const NATIVE_ORDER: u32 = 1;
pub(crate) const SUBMIT_POLLS: u32 = 500;
pub(crate) const PROBE_LOW_RANGE: u32 = 0;
pub(crate) const LATE_SKSM: u32 = 0;
pub(crate) const FLUSHID_FIX: u32 = 0;
/// Session compute FList identity; render uses the separate pool ID zero.
pub(crate) const COMPUTE_FLIST_ID: u16 = 1;

/// Published CL graphs retain distinct pools; render owns zero. The physical
/// QID lease makes this mapping stable across logical owner turnover.
pub(crate) const fn compute_flist_id(queue_id: u8) -> u16 {
    queue_id as u16 + 1
}
pub(crate) const SKIP: u32 = 0;
pub(crate) const DEFER_ADD_KICKS: u32 = 0;
pub(crate) const CTX2_ROOT: u32 = 3;
pub(crate) const PROGRESS_WATCHDOG: u32 = 1;
pub(crate) const POWER_HOLD: u32 = 0;
pub(crate) const POWER_WIRE: u32 = 0;
pub(crate) const IDLE_SELECTOR: u32 = 0;
pub(crate) const RENDER_ABSOLUTE_POINTERS: u32 = 0;
pub(crate) const RENDER_STATUS_FLAG: u32 = 0;
pub(crate) const RENDER_GATE_PROBE: u32 = 0;
pub(crate) const RENDER_KSM_PROBE: u32 = 0;
pub(crate) const RENDER_HEADER_FLAGS: u32 = 0;
pub(crate) const RENDER_GENERATION_BIAS: u32 = 0;
pub(crate) const RENDER_BG_PREFIX: u32 = 0;
pub(crate) const RENDER_EOT_BIND_ZERO: u32 = 1;
pub(crate) const RENDER_MCACHE_HWSID: u32 = 24;
pub(crate) const RENDER_MCACHE_MODE: u32 = 1;
pub(crate) const RENDER_MCACHE_SELECTOR: u32 = 64;
pub(crate) const RENDER_KSM_MCACHE: u32 = 1;
pub(crate) const RENDER_NO_BARRIER: u32 = 0;
pub(crate) const RENDER_SPLIT_LAUNCH: u32 = 0;
pub(crate) const RENDER_NATIVE_DOORBELLS: u32 = 0;
pub(crate) const RENDER_NATIVE_POOL_A_FIRST_RECORD: u32 = 1;
pub(crate) const RENDER_NATIVE_FRAGMENT_HWPB: u32 = 1;
pub(crate) const RENDER_3D_RCE: u32 = 0xff;
pub(crate) const SEED_WORK_CHANNEL: u32 = 0;
pub(crate) const RENDER_3D_STROBE: u32 = 0;
pub(crate) const IRQ_BURST: u32 = 0;
pub(crate) const SLOT_BURST: u32 = 0;
pub(crate) const PIPE_SWEEP: u32 = 0;
pub(crate) const RENDER_TA_DM: u32 = 0xff;
pub(crate) const RENDER_3D_DM: u32 = 0xff;
pub(crate) const FW_TRACE_CLASSES: u32 = 0;
pub(crate) const KSM_COLD_ARM: u32 = 0;
pub(crate) const ADMIT_ARM: u32 = 0;
pub(crate) const RENDER_NO_INSTALL: u32 = 0;
pub(crate) const FRAG_ORACLE: u32 = 0;
pub(crate) const TA_ORACLE: u32 = 0;
pub(crate) const RETAIN_STAGING: u32 = 1;
pub(crate) const RENDER_USC_FREELIST: u32 = 1;
pub(crate) const RECOVERY_ALLOW_ACTIVE_SCHEDULER: u32 = 1;
pub(crate) const RENDER_SERVICE_EVENTS: u32 = 1;
pub(crate) const COMPUTE_SKSM_ENTRY: u32 = 1;
pub(crate) const REPORT_DRAIN: u32 = 0;
pub(crate) const REPORT_DRAIN_BUDGET: u32 = 0;
pub(crate) const RENDER_RECOVERY_RETIRE: u32 = 0;
pub(crate) const COMPUTE_CONFIG_CONTEXT: u32 = 1;
// bit0: scope a faulted async-compute queue's admission poison to its own
//       client instead of the whole device (landed with f6a71e767d55a).
// bit1: scope a lost render submission the same way. When a firmware
//       stamp-blamed recovery is serviced to a clean final clear (state=0,
//       epoch=1) but the driver cannot prove the exact lost render was
//       firmware-retired, fail ONLY that client's VM and retire its physical
//       slot; keep the session up for every other client. Without this bit the
//       render settle path calls note_session_recovery_pending and the whole
//       GPU session is declared crashed -- and on G17P a crashed session's
//       rebuild never re-runs the RTKit HELLO handshake (4/4 ETIME), so the
//       GPU is dead until reboot. Measured 2026-09-26 (07:48, 07:54, and the
//       real desktop at 08:12): a single unprivileged client's first render
//       submission killed the GPU for everyone. Both scopings are the correct
//       fault-containment default, not a diagnostic opt-in: they never touch
//       the healthy path's batching or concurrency.
pub(crate) const QUARANTINE_SCOPE: u32 = 3;
pub(crate) const RENDER_NATIVE_REGS: u32 = 0;
pub(crate) const RENDER_NATIVE_BYTES: u32 = 0;
pub(crate) const RENDER_TVB: u32 = 2;
pub(crate) const DEP_RECORDS: u32 = 0;
pub(crate) const NO_IMPLICIT_DEP: u32 = 0;
pub(crate) const RENDER_TA_QID_FIELD: u32 = 1;
pub(crate) const RENDER_SLOT_TEMPLATE: u32 = 0;
pub(crate) const RENDER_SEED_TS: u32 = 0;
pub(crate) const RENDER_KICK_QOS: u32 = 0;
pub(crate) const RENDER_TAG16: u32 = 0;
pub(crate) const RENDER_POPULATED_HIGH_ROOT: u32 = 0;
pub(crate) const RENDER_SKSM: u32 = 3;
pub(crate) const IRQ_COMPLETION: u32 = 1;
pub(crate) const RING_NORMALIZE: u32 = 0;
pub(crate) const RING_LIMIT: u32 = 0;
pub(crate) const RING_WRAP: u32 = 0;
pub(crate) const COMPUTE_SCS_PAGES: u32 = 0;
pub(crate) const COMPUTE_SCS_MIB: u32 = 0;
pub(crate) const COMPUTE_SCS_EXTRAS: u32 = 0;
pub(crate) const COMPUTE_SCS_WINDOW: u32 = 0;
pub(crate) const COMPLETION_STAMP: u32 = 1;
pub(crate) const REPEAT_CONTEXT: u32 = 1;
pub(crate) const REPEAT_RING: u32 = 1;
pub(crate) const REPEAT_ACTIVATION: u32 = 0;
pub(crate) const QUEUE_RECYCLE: u32 = 2;
/// Diagnostic only: 0 allocates a fresh QID graph for each new async-compute
/// owner so retained graph rebinding can be isolated from execution faults.
/// Production must remain 1 once the handoff defect is fixed.
pub(crate) const ASYNC_COMPUTE_REBIND: u32 = 1;
/// Diagnostic only: retain the pre-publication B1 descriptor/FList snapshot
/// so the first halted fault can be compared with the exact bytes it consumed.
pub(crate) const COMPUTE_RESOURCE_SNAPSHOT: u32 = 0;
pub(crate) const ZERO_DURATION: u32 = 1;
pub(crate) const GATE_QUIESCE_MS: u32 = 0;
pub(crate) const RECOVERY_RESYNC: u32 = 1;
pub(crate) const ASC_RESTART: u32 = 0;
pub(crate) const ASC_GATE_SETTLE_MS: u32 = 10;
pub(crate) const ASC_TEARDOWN: u32 = 0;
pub(crate) const ABANDON_RECOVERY: u32 = 1;
pub(crate) const SKSM_MMIO: u32 = 0;

/// Default-off diagnostic: ordinary Job graphs with a shared per-submit TSQ
/// use native pending-IO-fence-free TA policy. This defers the early TA drain;
/// it does not remove maintenance or relax any post-maintenance fence gate.
/// Bootstrap/private-node paths retain their qualified policy. Do not promote
/// without mixed/failure/chunking liveness and throughput qualification.
pub(crate) const RENDER_NATIVE_TA_DRAIN_PROBE: bool = cfg!(g17p_native_ta_drain_probe);
