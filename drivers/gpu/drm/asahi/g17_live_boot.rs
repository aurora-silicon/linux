// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G17P live-boot lifecycle boundary.
//!
//! T8140 has two independent ASCWrap v6 providers. The provider owns the
//! wrapper and IOP VBAR mappings; DRM must never map an `asc1` alias or write
//! `CPU_RUN` directly. A previous stage did exactly that and then freed RTKit
//! transports while both CPUs could still be running.
//!
//! The current path keeps each hardware owner explicit:
//!
//! 1. [`crate::driver`] admits the exact live DT topology before any target
//!    effect and passes an unforgeable [`T8140LiveTopologyAdmission`] token.
//! 2. This module resolves both mailbox providers and verifies their roles and
//!    exact resources through the provider-owned lifecycle surface.
//! 3. Both providers report a matching CPU start and stop implementation.
//! 4. Both RTKit receivers are armed before CPU start. GFX completes its
//!    handshake before GFX1 starts.
//! 5. The retained runtime drops the registered queue, GFX1/GFX transports,
//!    provider CPUs, and manager resources in strict reverse order.

#![allow(dead_code)]

use core::{
    sync::atomic::{fence, AtomicBool, AtomicU32, Ordering},
};

use kernel::macros::vtable;
use kernel::new_condvar;
use kernel::{
    bindings,
    device::Core,
    error::{from_err_ptr, to_result},
    io::{Io, Mmio, MmioRaw},
    iosys_map::IoSysMapRef,
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::{Arc, CondVar, CondVarTimeoutResult},
    time::{delay::fsleep, Delta},
};
#[cfg(CONFIG_DEV_COREDUMP)]
use kernel::{
    devcoredump, new_mutex,
    sync::Mutex,
    time::{msecs_to_jiffies, Instant, Monotonic},
};

use crate::{
    driver::{AsahiDevice, T8140LiveTopologyAdmission},
    g17_completion, g17_initdata, g17_lifecycle, g17_manager, g17_resources, g17_rtkit,
    g17_submission, g17_uapi, gem, hw, identity, mmu, module_parameters, regs,
};
use kernel::sync::aref::ARef;

/// Ordered boundary stages.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum BootStage {
    ExactTopology,
    ProviderOwnership,
    ReversibleLifecycle,
    StartCpus,
    BootRtkit,
    PublishInitdata,
    InitdataAcknowledged,
    DeviceControlPublished,
    RegisterComputeQueue,
}

const INITDATA_ACK_TYPE: u16 = 0x09;
const INITDATA_ACK_POLLS: usize = 200;
const CONTROL_COUNTER_POLLS: usize = 100;
const FIRMWARE_RECOVERY_POLLS: usize = 500;
const G17P_CRASHLOG_MAX_BYTES: usize = 256 * 1024;
/// 1ms samples of `sgx+0xe01000 & 0xf` after the submit power record. Nothing
/// acknowledges the device-control power lever, so this is an observation
/// window, not a handshake: the firmware's own core-power sequencer
/// (`0x20a70`) budgets `0xe10` timebase ticks for its `sgx+0x2_11d0` poll.
const G17P_POWER_WIRE_POLLS: usize = 20;
const G17P_RTKIT_UAT_DVA_MASK: u64 = (1u64 << 40) - 1;

/// Module-local, read-only view of one existing ASCWrap mailbox provider.
///
/// The C provider owns the mapping and all lifecycle transitions. This view
/// neither maps an alias nor changes callback ownership; it only borrows the
/// provider's established mailbox-register pointer for two control reads.
struct G17PMailboxProvider {
    mbox: *mut bindings::apple_mbox,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PAscwrapV6Role {
    Gfx,
    Gfx1,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PAscwrapV6Lifecycle {
    role: G17PAscwrapV6Role,
    wrapper: (u64, u64),
    iop_vbar: (u64, u64),
    missing: u32,
}

impl G17PMailboxProvider {
    fn get(dev: &kernel::device::Device, index: usize) -> Result<Self> {
        // SAFETY: `dev` is live and apple_mbox_get() establishes a device link
        // to keep the resolved provider alive for this consumer.
        let mbox = unsafe {
            from_err_ptr(bindings::apple_mbox_get(
                dev.as_raw(),
                index.try_into()?,
            ))?
        };
        Ok(Self { mbox })
    }

    fn lifecycle(&self) -> Result<G17PAscwrapV6Lifecycle> {
        // M3/TB integration preserves the existing mailbox provider.
        Err(kernel::error::Error::from_errno(-(bindings::EOPNOTSUPP as i32)))
    }

    fn require_safe_cpu_lifecycle(&self) -> Result {
        // M3/TB integration preserves the existing mailbox provider.
        Err(kernel::error::Error::from_errno(-(bindings::EOPNOTSUPP as i32)))
    }

    fn start_cpu(&self) -> Result {
        // M3/TB integration preserves the existing mailbox provider.
        Err(kernel::error::Error::from_errno(-(bindings::EOPNOTSUPP as i32)))
    }

    fn stop_cpu(&self) -> Result {
        // M3/TB integration preserves the existing mailbox provider.
        Err(kernel::error::Error::from_errno(-(bindings::EOPNOTSUPP as i32)))
    }

    fn a2i_control(&self) -> Result<u32> {
        const A2I_CONTROL: usize = 0x110;
        const CONTROL_WINDOW_SIZE: usize = A2I_CONTROL + core::mem::size_of::<u32>();

        // SAFETY: the provider owns the live ASC mailbox mapping. The device
        // link established by apple_mbox_get() keeps it valid, and this view
        // is bounded to the standard read-only A2I control register.
        let raw = MmioRaw::<CONTROL_WINDOW_SIZE>::new(
            unsafe { (*self.mbox).regs as usize },
            CONTROL_WINDOW_SIZE,
        )?;
        // SAFETY: `raw` covers the complete 32-bit control register read.
        Ok(unsafe { Mmio::from_raw(&raw) }
            .relaxed()
            .read32(A2I_CONTROL))
    }

}

// SAFETY: the provider mapping is device-link protected and the reader issues
// only relaxed MMIO reads. Mailbox mutation remains synchronized in C.
unsafe impl Send for G17PMailboxProvider {}
// SAFETY: as above; concurrent snapshots do not change either FIFO.
unsafe impl Sync for G17PMailboxProvider {}

fn log_g17p_a2i_control(
    dev: &kernel::device::Device,
    mailbox: &G17PMailboxProvider,
    checkpoint: &'static str,
) {
    match mailbox.a2i_control() {
        Ok(control) => dev_info!(
            dev,
            "G17P A2I checkpoint={} raw={:#x} count={} empty={} full={} read={} write={} enabled={}\n",
            checkpoint,
            control,
            (control >> 20) & 0xf,
            (control >> 17) & 1,
            (control >> 16) & 1,
            (control >> 12) & 0xf,
            (control >> 8) & 0xf,
            control & 1,
        ),
        Err(error) => dev_err!(
            dev,
            "G17P A2I checkpoint={} read failed ({:?})\n",
            checkpoint,
            error,
        ),
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PWorkScanObservation {
    first: u32,
    last: u32,
    observed_or: u32,
    transitions: u32,
    samples: u32,
    failures: u32,
}

impl G17PWorkScanObservation {
    const fn new() -> Self {
        Self {
            first: 0,
            last: 0,
            observed_or: 0,
            transitions: 0,
            samples: 0,
            failures: 0,
        }
    }

    fn observe(&mut self, value: u32) {
        if self.samples == 0 {
            self.first = value;
        } else if self.last != value {
            self.transitions = self.transitions.saturating_add(1);
        }
        self.last = value;
        self.observed_or |= value;
        self.samples = self.samples.saturating_add(1);
    }

    fn observe_failure(&mut self) {
        self.failures = self.failures.saturating_add(1);
    }
}

/// Accumulated KTrace drain outcome for one role across a submit poll loop.
#[derive(Debug, Copy, Clone, Default)]
struct G17PKtraceTotals {
    consumed: u32,
    printed: u32,
    skipped: u32,
    lost_records: u64,
    lost_windows: u32,
    saw_add_kicks: bool,
    last_pause_mask: u64,
    last_producer: u32,
}

impl G17PKtraceTotals {
    fn accumulate(&mut self, stats: &g17_resources::G17PKtraceDrainStats) {
        self.consumed = self.consumed.saturating_add(stats.consumed);
        self.printed = self.printed.saturating_add(stats.printed);
        self.skipped = self.skipped.saturating_add(stats.skipped);
        self.lost_records = self.lost_records.saturating_add(stats.lost_records);
        self.lost_windows = self.lost_windows.saturating_add(stats.lost_windows);
        self.saw_add_kicks |= stats.saw_add_kicks;
        if stats.last_pause_mask != 0 {
            self.last_pause_mask = stats.last_pause_mask;
        }
        self.last_producer = stats.producer;
    }
}

/// The KTrace drain is deliberately gated on the same parameter that arms the
/// class mask: arming without a consumer makes the firmware busy-wait once and
/// then silently drop records, so "armed but undrained" must be impossible.
fn g17p_ktrace_enabled() -> bool {
    *module_parameters::g17p_fw_trace.value() != 0
}

fn g17p_ktrace_role_name(role: g17_initdata::InstanceRole) -> &'static str {
    match role {
        g17_initdata::InstanceRole::Primary => "primary",
        g17_initdata::InstanceRole::Secondary => "secondary",
    }
}

/// Checkpoint probe: dump the firmware log ring. DRAM only, like the KTrace
/// drain -- neither touches the sgx window, so both are safe on a path where
/// the GPU cores may never have powered.
fn dump_g17p_firmware_log(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    checkpoint: &'static str,
) {
    if *module_parameters::g17p_fw_log.value() == 0 {
        return;
    }
    match manager.dump_firmware_log(60) {
        Ok(count) => dev_info!(
            dev,
            "G17P fwlog checkpoint={} records={}\n",
            checkpoint,
            count
        ),
        Err(error) => dev_warn!(
            dev,
            "G17P fwlog checkpoint={} dump failed ({:?})\n",
            checkpoint,
            error
        ),
    }
}

/// Checkpoint probe: verify the published channel-14 pointer pair and drain
/// whatever the firmware has already traced. Note this consumes what it reads.
fn probe_g17p_ktrace(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    checkpoint: &'static str,
) {
    if !g17p_ktrace_enabled() {
        return;
    }
    let verbose = *module_parameters::g17p_fw_ktrace_verbose.value() != 0;
    let budget = *module_parameters::g17p_fw_ktrace_budget.value();
    for role in [
        g17_initdata::InstanceRole::Primary,
        g17_initdata::InstanceRole::Secondary,
    ] {
        let role_name = g17p_ktrace_role_name(role);
        match manager.check_ktrace_ring_pointers(role) {
            Ok((state, ring)) => dev_info!(
                dev,
                "G17P ktrace pointers checkpoint={} role={} state={:#x} ring={:#x}\n",
                checkpoint,
                role_name,
                state,
                ring
            ),
            Err(error) => dev_warn!(
                dev,
                "G17P ktrace pointers checkpoint={} role={} check failed ({:?})\n",
                checkpoint,
                role_name,
                error
            ),
        }
        match manager.drain_ktrace_ring(role, verbose, budget) {
            Ok(stats) => dev_info!(
                dev,
                "G17P ktrace armed check checkpoint={} role={} producer-snapshot={} consumed-count={} consumer-before={} consumer-after={} printed={} skipped={} lost={}/{} addkicks={} pause-mask={:#x}\n",
                checkpoint,
                role_name,
                stats.producer,
                stats.consumed,
                stats.consumer_before,
                (stats.consumer_before + stats.consumed) % g17_completion::G17P_KTRACE_CAPACITY,
                stats.printed,
                stats.skipped,
                stats.lost_records,
                stats.lost_windows,
                stats.saw_add_kicks,
                stats.last_pause_mask
            ),
            Err(error) => dev_warn!(
                dev,
                "G17P ktrace armed check checkpoint={} role={} failed ({:?})\n",
                checkpoint,
                role_name,
                error
            ),
        }
    }
}

/// Checkpoint probe: read the firmware EVENT ring (channel-table entry 13).
///
/// Read-only unless `g17p_report_drain` is 2. This ring is where the firmware
/// asks the host to grow the tiling parameter buffer (event type 7), and the
/// G17P path has never read it, so a request would sit in DRAM unanswered
/// while the tiler waited. DRAM only -- no sgx MMIO, so it is safe at both
/// checkpoints including the timeout, where the cores have powered back down.
fn probe_g17p_reports(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    checkpoint: &'static str,
) {
    let mode = *module_parameters::g17p_report_drain.value();
    if mode == 0 {
        return;
    }
    let advance = mode >= 2;
    let budget = *module_parameters::g17p_report_drain_budget.value();
    for role in [
        g17_initdata::InstanceRole::Primary,
        g17_initdata::InstanceRole::Secondary,
    ] {
        let role_name = g17p_ktrace_role_name(role);
        match manager.check_report_ring_pointers(role) {
            Ok((state, ring)) => dev_info!(
                dev,
                "G17P report pointers checkpoint={} role={} state={:#x} ring={:#x}\n",
                checkpoint,
                role_name,
                state,
                ring
            ),
            Err(error) => dev_warn!(
                dev,
                "G17P report pointers checkpoint={} role={} check failed ({:?})\n",
                checkpoint,
                role_name,
                error
            ),
        }
        match manager.drain_report_ring(role, advance, budget) {
            Ok(stats) => dev_info!(
                dev,
                "G17P report ring checkpoint={} role={} producer={} consumer={} pending={} read={} printed={} undecoded={} grow-requests={} restart-requests={} out-of-range={} advanced={} fwlog-write={}\n",
                checkpoint,
                role_name,
                stats.producer,
                stats.consumer_before,
                stats.producer.wrapping_sub(stats.consumer_before) & 0xff,
                stats.consumed,
                stats.printed,
                stats.skipped,
                stats.grow_requests,
                stats.restart_requests,
                stats.cursor_out_of_range,
                advance,
                stats.fwlog_write
            ),
            Err(error) => dev_warn!(
                dev,
                "G17P report ring checkpoint={} role={} failed ({:?})\n",
                checkpoint,
                role_name,
                error
            ),
        }
    }
}

fn log_g17p_status_a(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    checkpoint: &'static str,
) {
    match manager.status_a_snapshot() {
        Ok(snapshot) => dev_info!(
            dev,
            "G17P Status-A checkpoint={} scan-address={:#x} scan-active={} runtime-address={:#x} raw={:#x} active={} scheduler-constructed={}\n",
            checkpoint,
            snapshot.scan_address,
            snapshot.scan_active,
            snapshot.address,
            snapshot.raw,
            snapshot.active,
            snapshot.scheduler_constructed,
        ),
        Err(error) => dev_err!(
            dev,
            "G17P Status-A checkpoint={} read failed ({:?})\n",
            checkpoint,
            error,
        ),
    }
}

fn log_g17p_firmware_recovery_handshake(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    checkpoint: &'static str,
) {
    match manager.firmware_recovery_handshake_snapshot() {
        Ok(snapshot) => dev_info!(
            dev,
            "G17P firmware-recovery handshake checkpoint={} status-b={:#x} epoch-address={:#x} epoch={} state-address={:#x} state={} callback-known-unregistered={} waiting-first-response={} waiting-final-clear={} power-callback-count-address={:#x} power-callback-count={}\n",
            checkpoint,
            snapshot.status_b,
            snapshot.epoch_address,
            snapshot.epoch,
            snapshot.state_address,
            snapshot.state,
            snapshot.callback_known_unregistered(),
            snapshot.waiting_for_first_response(),
            snapshot.waiting_for_final_clear(),
            snapshot.power_callback_count_address,
            snapshot.power_callback_count,
        ),
        Err(error) => dev_err!(
            dev,
            "G17P firmware-recovery handshake checkpoint={} read failed ({:?})\n",
            checkpoint,
            error,
        ),
    }
}

/// Log the firmware's own fault report next to the MMU fault bank.
///
/// `main_config + 0x26c` is bundle view 3, so the report is in host memory we
/// already own; the firmware names the blamed kick queue there, that queue's
/// data master, and the recovery state. Nothing else in the driver reads it.
fn log_g17p_fault_report(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    checkpoint: &'static str,
) {
    // This report is validated host-owned WC memory, not the diagnostic MMIO
    // fault bank. Keep it available with register probing disabled, especially
    // before recovery acknowledgement clears the firmware's blame fields.
    match manager.firmware_fault_report_snapshot() {
        Ok(snapshot) => dev_info!(
            dev,
            "G17P fault report checkpoint={} report={:#x} state={} host-requested={} reason={} sources={} blamed-qid={}/{} ksm-slot={}/{} data-master={}/{}\n",
            checkpoint,
            snapshot.report,
            snapshot.state,
            snapshot.host_requested,
            snapshot.reason,
            snapshot.source_count,
            snapshot.qid_present,
            snapshot.qid,
            snapshot.slot_present,
            snapshot.slot,
            snapshot.data_master_present,
            snapshot.data_master,
        ),
        Err(error) => dev_warn!(
            dev,
            "G17P fault report checkpoint={} read failed ({:?})\n",
            checkpoint,
            error,
        ),
    }
    // Keep this independent of the report read: failure of either bounded
    // host-memory diagnostic must not suppress the other or touch MMIO.
    match manager.firmware_dm1_slot0_snapshot() {
        Ok(snapshot) => dev_info!(
            dev,
            "G17P DM1 slot0 monitor checkpoint={} address={:#x} raw={:08x?} monitor-state={} monitor-timestamp={:#x}\n",
            checkpoint,
            snapshot.address,
            snapshot.raw,
            snapshot.state(),
            snapshot.timestamp(),
        ),
        Err(error) => dev_warn!(
            dev,
            "G17P DM1 slot0 monitor checkpoint={} read failed ({:?})\n",
            checkpoint,
            error,
        ),
    }
}

/// A narrowly scoped, already-halted slot discriminator. Failure to observe
/// it must never prevent the host acknowledgement that resumes recovery.
fn log_g17p_halted_dm1_slot0(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    registers: Option<&regs::Resources>,
    expected_epoch: u64,
) {
    let Some(registers) = registers else { return };
    let selected = match (
        manager.firmware_recovery_handshake_snapshot(),
        manager.firmware_fault_report_snapshot(),
    ) {
        (Ok(handshake), Ok(report)) => {
            g17_completion::G17PHaltedSlot0Gate {
                handshake_state: handshake.state,
                epoch: handshake.epoch,
                expected_epoch,
                report_state: report.state,
                host_requested: report.host_requested,
                slot_present: report.slot_present,
                slot: report.slot,
                data_master_present: report.data_master_present,
                data_master: report.data_master,
            }.allows_sample()
        }
        _ => false,
    };
    if !selected {
        return;
    }
    match registers.g17p_halted_slot0_snapshot() {
        Ok(Some(snapshot)) => {
            let stable = snapshot.state_before == snapshot.state_after;
            dev_info!(
                dev,
                "G17P halted DM1 slot0 epoch={} gpc={:#x} c020-before={:#018x} c078={:#018x} c020-after={:#018x} stable={} progress-selector={} selector={} key-qid={} key-stamp={:#x}\n",
                expected_epoch,
                snapshot.gpc_state,
                snapshot.state_before,
                snapshot.key,
                snapshot.state_after,
                stable,
                g17_completion::g17p_slot_has_progress_selector(snapshot.state_before),
                (snapshot.state_before >> 24) & 3,
                (snapshot.key >> 40) & 0x7f,
                snapshot.key & 0xff_ffff_ffff,
            );
            if let Some(selector) = g17_completion::g17p_dm1_monitor_selector_index(snapshot.state_before) {
                dev_info!(
                    dev,
                    "G17P halted DM1 selected-progress epoch={} selector-index={} c020-stable={} count={}/8 errno={} regs=1668/13020/15150/15388/15390/15510/15518/a068 raw={:016x?}\n",
                    expected_epoch,
                    selector,
                    stable,
                    snapshot.progress_count,
                    snapshot.progress_errno,
                    &snapshot.progress[..snapshot.progress_count],
                );
            }
        }
        Ok(None) => dev_info!(dev, "G17P halted DM1 slot0: GPC off; no low-SGX access\n"),
        Err(error) => dev_warn!(dev, "G17P halted DM1 slot0: read failed ({:?}); continuing recovery\n", error),
    }
}

/// Preserve the fault selected by B1's reason-3 classifier before ACK. This
/// does not use Status-B's copied slot mask as a GMMU-fault predicate.
fn log_g17p_halted_gmmu(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    registers: Option<&regs::Resources>,
    expected_epoch: u64,
) {
    let Some(registers) = registers else { return };
    let allowed = match (
        manager.firmware_recovery_handshake_snapshot(),
        manager.firmware_fault_report_snapshot(),
    ) {
        (Ok(handshake), Ok(report)) => g17_completion::g17p_halted_gmmu_sample_allowed(
            handshake.state, handshake.epoch, expected_epoch,
            report.state, report.host_requested, report.reason,
        ),
        _ => false,
    };
    if !allowed {
        return;
    }
    match registers.g17p_halted_gmmu_snapshot() {
        Ok(Some(snapshot)) => {
            let stable = snapshot.info_before == snapshot.info_after;
            if let Some(address_word) = snapshot.address_word {
                let address = address_word << 6;
                dev_info!(
                    dev,
                    "G17P halted GMMU reason3 epoch={} gpc={:#x} info-before={:#018x} addr-word={:#018x} va={:#x} info-after={:#018x} stable={} reason={} read={} level={} unit={:#x} vm-slot={}\n",
                    expected_epoch, snapshot.gpc_state, snapshot.info_before,
                    address_word, address, snapshot.info_after, stable,
                    (snapshot.info_before >> 1) & 7,
                    (snapshot.info_before >> 4) & 1,
                    (snapshot.info_before >> 7) & 3,
                    (snapshot.info_before >> 9) & 0xff,
                    (snapshot.info_before >> 17) & 0x3f,
                );
                manager.log_fault_address_candidates("halted-reason3", address_word);
            } else {
                dev_info!(
                    dev,
                    "G17P halted GMMU reason3 epoch={} gpc={:#x} info-before={:#018x} info-after={:#018x} stable={} no-valid-fault; address not read\n",
                    expected_epoch, snapshot.gpc_state,
                    snapshot.info_before, snapshot.info_after, stable,
                );
            }
        }
        Ok(None) => dev_info!(dev, "G17P halted GMMU reason3: GPC off; no low-SGX access\n"),
        Err(error) => dev_warn!(dev, "G17P halted GMMU reason3: read failed ({:?}); continuing recovery\n", error),
    }
}

fn validate_primary_bootstrap_uma_completion(
    pdev: &platform::Device,
    record: &g17_resources::G17PPrimaryFirmwareEventRecord,
) -> Result<g17_manager::G17PBootstrapUmaCompletion> {
    let uma = g17_completion::decode_g17p_firmware_uma_grow_event(&record.raw)
        .map_err(|_| EIO)?;
    let expected_operand_table = g17_initdata::CONTROL_OPERAND_TABLE_ADDRESS;
    let expected_cookie = g17_submission::G17P_USC_FREELIST_LOW_END;
    if uma.stamp_index != 1
        || uma.pool_index != 0
        || uma.shared_control != g17_initdata::CONTROL_SHARED_ADDRESS
        || uma.operand_table != expected_operand_table
        || uma.cookie != expected_cookie
        || uma.result != g17_submission::G17P_USC_FREELIST_GROW_DESCRIPTOR_BYTES
    {
        dev_err!(
            pdev.as_ref(),
            "G17P UMA grow: rejected bootstrap completion stamp={} pool={} shared={:#x} operand={:#x} cookie={:#x} result={:#x}\n",
            uma.stamp_index,
            uma.pool_index,
            uma.shared_control,
            uma.operand_table,
            uma.cookie,
            uma.result
        );
        return Err(EIO);
    }
    Ok(g17_manager::G17PBootstrapUmaCompletion {
        stamp: uma.stamp_index,
        hardware_buffer_id: uma.pool_index,
        shared_control: uma.shared_control,
        operand_table: uma.operand_table,
        cookie: uma.cookie,
        count: uma.result,
    })
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PPrimaryFirmwareEventBatch {
    recovery_generation: Option<u64>,
    recovery_records: u32,
    uma_release: Option<g17_manager::G17PBootstrapUmaRelease>,
    blamed_stamp: Option<i32>,
    unknown_records: u32,
    completion_records: u32,
    blamed_recovery_records: u32,
    notification_count: u32,
}

fn drain_primary_firmware_event_batch(
    pdev: &platform::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    state: &G17PRoleRtkitState,
    signal: &G17PCompletionSignal,
) -> Result<G17PPrimaryFirmwareEventBatch> {
    let notification_count = state
        .firmware_event_notifications
        .swap(0, Ordering::AcqRel);
    let mut batch = G17PPrimaryFirmwareEventBatch {
        recovery_generation: None,
        recovery_records: 0,
        uma_release: None,
        blamed_stamp: None,
        unknown_records: 0,
        completion_records: 0,
        blamed_recovery_records: 0,
        notification_count,
    };
    let role = match state.processor {
        G17PProcessor::Gfx => g17_initdata::InstanceRole::Primary,
        G17PProcessor::Gfx1 => g17_initdata::InstanceRole::Secondary,
    };
    for _ in 0..g17_completion::G17P_FIRMWARE_EVENT_CAPACITY {
        let Some(record) = manager.drain_firmware_event(role)? else { break; };
        let event_type = u32::from_le_bytes(record.raw[0..4].try_into().map_err(|_| EIO)?);
        match event_type {
            g17_completion::G17P_FIRMWARE_EVENT_RECOVERY_TYPE => {
                let recovery =
                    g17_completion::decode_g17p_firmware_recovery_event(&record.raw)
                        .map_err(|_| EIO)?;
                if recovery.stamp_index != -1 {
                    // A stamp-blamed recovery is a real firmware request, not a
                    // malformed record. Returning EIO here aborted the whole
                    // batch drain and, because the caller treats that as a hard
                    // failure, left every later record in the ring unreachable --
                    // the same stall that unmodelled record types used to cause.
                    // Report it and keep draining; the recovery bookkeeping below
                    // is only valid for the empty (stamp -1) form, so skip it.
                    // Do NOT skip this. Draining past a stamp-blamed recovery
                    // fixed the ring stall but broke the handshake: skipping left
                    // `recovery_generation == None`, and every caller keys off
                    // that field, so nothing ever scheduled the recovery. The
                    // firmware raises halt unconditionally and then spins until
                    // the host writes 2 into status_b[0x4900]; never answering
                    // leaves KSM halted forever, which is the ETIMEDOUT we saw.
                    // Fall through to the same generation bookkeeping the empty
                    // form gets -- those checks are stamp-independent.
                    dev_err!(
                        pdev.as_ref(),
                        "G17P recovery: firmware requests stamp-blamed recovery generation={} stamp={}; servicing\n",
                        recovery.generation,
                        recovery.stamp_index
                    );
                    batch.blamed_recovery_records =
                        batch.blamed_recovery_records.saturating_add(1);
                    batch.blamed_stamp = Some(recovery.stamp_index);
                }
                dev_info!(
                    pdev.as_ref(),
                    "G17P event worker: type4 generation={} cursor={}->{} producer={}\n",
                    recovery.generation,
                    record.consumer_before,
                    record.consumer_published,
                    record.producer_snapshot
                );
                if let Err(error) =
                    manager.validate_empty_primary_firmware_recovery(recovery.generation)
                {
                    dev_err!(
                        pdev.as_ref(),
                        "G17P event worker: type4 generation={} failed host-generation/empty-scheduler validation ({:?})\n",
                        recovery.generation,
                        error
                    );
                    return Err(error);
                }
                if batch
                    .recovery_generation
                    .is_some_and(|generation| generation != recovery.generation)
                {
                    dev_err!(
                        pdev.as_ref(),
                        "G17P recovery: one IRQ batch contains different generations {:?} and {}\n",
                        batch.recovery_generation,
                        recovery.generation
                    );
                    return Err(EIO);
                }
                batch.recovery_generation = Some(recovery.generation);
                batch.recovery_records = batch.recovery_records.checked_add(1).ok_or(EOVERFLOW)?;
                break;
            }
            g17_completion::G17P_FIRMWARE_EVENT_UMA_GROW_TYPE => {
                let completion = validate_primary_bootstrap_uma_completion(pdev, &record)?;
                manager
                    .match_render_usc_freelist_completion(completion)
                    .map_err(|error| {
                        dev_err!(
                            pdev.as_ref(),
                            "G17P UMA grow: type13 has no matching live FList owner ({:?})\n",
                            error
                        );
                        error
                    })?;
                dev_info!(
                    pdev.as_ref(),
                    "G17P UMA grow: matched retained FList owner stamp={} ID={} cookie={:#x} count={:#x}; retaining TA/3D references until paired completion\n",
                    completion.stamp,
                    completion.hardware_buffer_id,
                    completion.cookie,
                    completion.count,
                );
            }
            g17_completion::G17P_FIRMWARE_EVENT_COMPLETION_TYPE => {
                // Type 1 is published exactly once per executed submission, but
                // only its first 0x18 bytes are real: everything beyond that is
                // the 8-byte repeating poison "RTKSTACK". An earlier reading of
                // uuid/context/stamp at +0x28..+0x40 was KTrace spill into the
                // same pages -- with KTrace disabled those offsets are pure
                // poison, which is how the misread was caught. Log the real
                // prefix only; do NOT synthesise a completion from bytes whose
                // encoding is not yet established.
                let qword = |offset: usize| {
                    u64::from_le_bytes(record.raw[offset..offset + 8].try_into().unwrap_or([0; 8]))
                };
                dev_info!(
                    pdev.as_ref(),
                    "G17P completion record: head={:#x} {:#x} {:#x} cursor={}->{} producer={}\n",
                    qword(0x00),
                    qword(0x08),
                    qword(0x10),
                    record.consumer_before,
                    record.consumer_published,
                    record.producer_snapshot
                );
                batch.completion_records = batch.completion_records.saturating_add(1);
            }
            unknown => {
                // Only recovery (type 4) and UMA-grow (type 13) were ever
                // reachable while the firmware never executed work. Now that a
                // kick actually runs, the firmware publishes further record
                // kinds here; rejecting them with EIO aborted the whole drain
                // and left the submission unretired. Log the record and keep
                // draining -- `drain_primary_firmware_event` has already
                // published the consumer cursor for it.
                batch.unknown_records = batch.unknown_records.saturating_add(1);
                if batch.unknown_records <= 4 {
                    dev_info!(
                        pdev.as_ref(),
                        "G17P event worker: unhandled firmware event type={} cursor={}->{} producer={} raw={:02x?}\n",
                        unknown,
                        record.consumer_before,
                        record.consumer_published,
                        record.producer_snapshot,
                        &record.raw[..32.min(record.raw.len())]
                    );
                }
            }
        }
    }
    if batch.unknown_records != 0 {
        dev_info!(
            pdev.as_ref(),
            "G17P event worker: {} unhandled firmware event record(s) this batch\n",
            batch.unknown_records
        );
    }
    if batch.recovery_records != 0 {
        dev_info!(
            pdev.as_ref(),
            "G17P event worker: notifications={} type4={} generation={:?}\n",
            batch.notification_count,
            batch.recovery_records,
            batch.recovery_generation,
        );
    }
    Ok(batch)
}

fn complete_primary_real_recovery(
    pdev: &platform::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    registers: Option<&regs::Resources>,
    state: &G17PRoleRtkitState,
    generation: u64,
    duplicate_records: u32,
) -> Result {
    let handshake = manager.firmware_recovery_handshake_snapshot()?;
    if handshake.state != 1 {
        dev_err!(
            pdev.as_ref(),
            "G17P recovery: type-4 generation={} arrived at shared epoch={} state={}, expected state 1\n",
            generation,
            handshake.epoch,
            handshake.state
        );
        return Err(EIO);
    }
    manager.validate_empty_primary_firmware_recovery(generation)?;
    // The scheduler-empty precondition is UNSATISFIABLE for a render-blamed
    // recovery. The firmware raises the restart request during the render
    // wait, so `render_storage`/`render_tracker` are populated by construction
    // and this returns EBUSY every time -- measured, after the inline drain
    // started reaching it. The guard was written when only compute could
    // trigger a recovery, and for compute the scheduler genuinely was empty.
    //
    // The precedent for relaxing it is a few lines below, in this same
    // function: the non-empty recovery-info table used to abort here too,
    // until it was understood that "refusing to clear is strictly worse than
    // clearing" -- refusing leaves the firmware pinned waiting for the host
    // forever. The same argument holds exactly: never answering leaves KSM
    // halted, which is the ETIMEDOUT.
    //
    // What the guard actually protects is the generation-0 SKSM timestamp
    // wipe, which would erase a live render's progress markers. So scope the
    // refusal to that wipe rather than to the whole handshake: acknowledge and
    // clear regardless, skip the wipe while render state is live.
    let scheduler_active = manager.render_scheduler_active();
    if scheduler_active {
        if *module_parameters::g17p_recovery_allow_active_scheduler.value() == 0 {
            manager.validate_empty_primary_firmware_recovery_scheduler()?;
        }
        dev_warn!(
            pdev.as_ref(),
            "G17P recovery: servicing generation={} with a LIVE render scheduler; the generation-0 SKSM timestamp wipe is skipped so the render's progress markers survive\n",
            generation
        );
    }
    dev_info!(
        pdev.as_ref(),
        "G17P recovery: servicing generation={} coalesced-records={} shared-epoch={} state={}\n",
        generation,
        duplicate_records,
        handshake.epoch,
        handshake.state
    );
    log_g17p_fault_report(pdev.as_ref(), manager, "recovery-blamed-cause");
    log_g17p_halted_dm1_slot0(pdev.as_ref(), manager, registers, handshake.epoch);
    log_g17p_halted_gmmu(pdev.as_ref(), manager, registers, handshake.epoch);
    let cause = manager.primary_firmware_recovery_cause_snapshot()?;
    dev_info!(
        pdev.as_ref(),
        "G17P recovery cause: zero0={:#x} detected-source-count={} zero1={:#x} command-word={:#x} raw48cc={:#x} raw48d0={:#x} fault-status={:#x} host-recovery={}\n",
        cause.zero_0,
        cause.detected_source_count,
        cause.zero_1,
        cause.command_word,
        cause.raw_48cc,
        cause.raw_48d0,
        cause.fault_status,
        cause.host_recovery
    );
    if cause.fault_status != 0 && *module_parameters::g17p_fault_report.value() != 0 {
        let mut faulting_addr_word = None;
        if let Some(registers) = registers {
            match registers.g17p_render_fault_irq_snapshot() {
                Ok(Some(snapshot)) => {
                    if snapshot.fault_info & 1 != 0 {
                        faulting_addr_word = Some(snapshot.fault_addr_word);
                    }
                    dev_info!(
                    pdev.as_ref(),
                    "G17P fault IRQ @recovery-state-1: gpc-state={:#x} info={:#018x} addr-word={:#018x} gate={:#018x} sub-status={:#018x} irq-status={:#018x} bit35={}\n",
                    snapshot.gpc_state,
                    snapshot.fault_info,
                    snapshot.fault_addr_word,
                    snapshot.requestor_gate,
                    snapshot.sub_status,
                    snapshot.irq_status,
                    (snapshot.irq_status >> 35) & 1,
                    )
                }
                Ok(None) => dev_warn!(
                    pdev.as_ref(),
                    "G17P fault IRQ @recovery-state-1: skipped because gpc-state=0\n"
                ),
                Err(error) => dev_warn!(
                    pdev.as_ref(),
                    "G17P fault IRQ @recovery-state-1: snapshot failed ({:?})\n",
                    error,
                ),
            }
            if let Err(error) =
                registers.g17p_log_fault_requestors_pre_ack("recovery-state-1")
            {
                dev_warn!(
                    pdev.as_ref(),
                    "G17P recovery: pre-ack fault-requestor sweep failed ({:?})\n",
                    error
                );
            }
        }
        if let Some(addr_word) = faulting_addr_word {
            manager.log_fault_address_candidates("recovery-state-1", addr_word);
        }
    }
    if scheduler_active {
        manager.log_render_recovery_pre_ack("recovery-state-1");
    }
    if cause.host_recovery != 0 {
        dev_err!(
            pdev.as_ref(),
            "G17P recovery: event-backed path has host-recovery flag {}\n",
            cause.host_recovery
        );
        return Err(EIO);
    }
    let accepted_generation = manager.accept_empty_primary_firmware_recovery(generation)?;
    dev_info!(
        pdev.as_ref(),
        "G17P recovery: accepted host generation {} before empty scheduler recovery\n",
        accepted_generation
    );

    // Only the first recovery of a session may wipe the SKSM last-submitted
    // timestamps. Doing it on every recovery erases the queue's progress and
    // makes the firmware immediately request another one: observed on J700 as
    // 105 identical fault-free recoveries (cause word 0x3, no fault, no
    // detected source) inside a single 625ms submit window.
    if generation == 0 && !scheduler_active {
        if let Err(error) = manager.dump_primary_recovery_cause_block() {
            dev_warn!(pdev.as_ref(), "G17P recovery: cause-block dump failed ({:?})\n", error);
        }
        manager.reset_empty_sksm_last_submitted_hw_timestamps()?;
        dev_info!(
            pdev.as_ref(),
            "G17P recovery: published 128 invalid/zero last-submitted HW timestamps\n"
        );
    }
    // Full binary observation while the validated state-1 halt still holds.
    // The archive owns copies; exposing it does not expose mutable GPU memory.
    finish_g17p_render_trace(pdev.as_ref(), manager, crate::g17_trace_capture::Phase::RecoveryBeforeAck);
    manager.acknowledge_primary_firmware_recovery(1, 2)?;
    let mut post_state = 2;
    for _ in 0..FIRMWARE_RECOVERY_POLLS {
        post_state = manager.firmware_recovery_handshake_snapshot()?.state;
        if post_state == 3 {
            break;
        }
        if state.crashed.load(Ordering::Acquire) {
            return Err(EIO);
        }
        fsleep(Delta::from_millis(1));
    }
    if post_state != 3 {
        dev_err!(
            pdev.as_ref(),
            "G17P recovery: firmware stopped at state {}, expected post-recovery state 3\n",
            post_state
        );
        return Err(ETIMEDOUT);
    }
    let recovery_info = manager.primary_firmware_recovery_info_snapshot()?;
    dev_info!(
        pdev.as_ref(),
        "G17P recovery info: emitted={} valid={} first-valid-index={:?} first-valid-flags={:#x}\n",
        recovery_info.emitted_entry_count,
        recovery_info.valid_entry_count,
        recovery_info.first_valid_index,
        recovery_info.first_valid_flags
    );
    // A non-empty recovery-info table used to abort here, back when only the
    // empty prepublication case was understood. That left the firmware pinned
    // at state 3 ("waiting for final clear") forever, because the final
    // acknowledge below is what clears it. On J700 the firmware emits exactly
    // one valid entry on the first real submission, with no fault recorded
    // (fault-status 0, host-recovery 0), so refusing to clear is strictly
    // worse than clearing: log the table and complete the handshake.
    if recovery_info.emitted_entry_count != 0 || recovery_info.valid_entry_count != 0 {
        dev_warn!(
            pdev.as_ref(),
            "G17P recovery: completing final clear with a non-empty recovery-info table (emitted={} valid={} first-index={:?} first-flags={:#x})\n",
            recovery_info.emitted_entry_count,
            recovery_info.valid_entry_count,
            recovery_info.first_valid_index,
            recovery_info.first_valid_flags
        );
        // The generation-0 dump above runs BEFORE the firmware reaches state
        // 3, so it always caught this table empty. Re-dump it now that the
        // firmware has published: each entry is 8 bytes and the driver's
        // snapshot only decodes the first word, so the second word of entry 0
        // -- the half that plausibly names what was blamed -- has never been
        // looked at. Host memory only; no MMIO.
        if let Err(error) = manager.dump_primary_recovery_cause_block() {
            dev_warn!(
                pdev.as_ref(),
                "G17P recovery: populated cause-block dump failed ({:?})\n",
                error
            );
        }
    }
    manager.acknowledge_primary_firmware_recovery(3, 0)?;
    let completed = manager.firmware_recovery_handshake_snapshot()?;
    dev_info!(
        pdev.as_ref(),
        "G17P recovery: final clear acknowledged; state={} epoch={}\n",
        completed.state,
        completed.epoch
    );
    if completed.state != 0 {
        return Err(EIO);
    }
    Ok(())
}

static INLINE_RECOVERIES_SERVICED: AtomicU32 = AtomicU32::new(0);

/// Offer only a sealed first-render archive through the established coredump
/// mechanism. Existing coredumps can occupy the device slot; the host must
/// verify the G17TRC01 magic rather than assume every devcd contains this data.
fn finish_g17p_render_trace(
    dev: &kernel::device::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    phase: crate::g17_trace_capture::Phase,
) {
    #[cfg(CONFIG_DEV_COREDUMP)]
    match manager.finish_render_trace(phase) {
        Ok(Some(capture)) => {
            let size = capture.len();
            devcoredump::dev_coredump(dev, &crate::THIS_MODULE, capture, GFP_KERNEL,
                msecs_to_jiffies(60 * 60 * 1000));
            dev_info!(dev, "G17P binary capture: sealed {} bytes phase={:?}; offered to devcoredump (G17TRC01)\n", size, phase);
        }
        Ok(None) => {}
        Err(error) => dev_warn!(dev, "G17P binary capture finalization failed: {:?}\n", error),
    }
}

fn settle_primary_empty_recovery(
    pdev: &platform::Device,
    manager: &mut g17_manager::G17PManagerConstruction,
    state: &G17PRoleRtkitState,
    signal: &G17PCompletionSignal,
) -> Result<Option<g17_manager::G17PBootstrapUmaRelease>> {
    let mut uma_release = None;
    for _ in 0..g17_completion::G17P_FIRMWARE_EVENT_CAPACITY {
        let batch = drain_primary_firmware_event_batch(pdev, manager, state, &signal)?;
        if let Some(release) = batch.uma_release {
            if uma_release.replace(release).is_some() {
                return Err(EIO);
            }
        }
        if let Some(generation) = batch.recovery_generation {
            manager.queue_primary_recovery(generation, batch.recovery_records)?;
            return Ok(uma_release);
        }
        let handshake = manager.firmware_recovery_handshake_snapshot()?;
        if handshake.state == 0 {
            return Ok(uma_release);
        }
        if handshake.state == 1 {
            // Firmware publishes type4 before state 1, but the producer can
            // race this snapshot. Reload without running recovery inline.
            continue;
        }
        dev_err!(
            pdev.as_ref(),
            "G17P event worker: shared epoch={} state={} has no schedulable type-4 record\n",
            handshake.epoch,
            handshake.state
        );
        return Err(EIO);
    }
    Err(EIO)
}

fn log_g17p_scheduler_gate(
    dev: &kernel::device::Device,
    registers: &regs::Resources,
    checkpoint: &'static str,
) {
    match registers.g17p_scheduler_gate_registers() {
        Ok(snapshot) => dev_info!(
            dev,
            "G17P scheduler gate checkpoint={} fender={:#x} armed={} gpc-state={:#x} host-irq-enable={:#x} host-irq-status={:#x}\n",
            checkpoint,
            snapshot.fender_dynamic_gating,
            snapshot.fender_wake_armed(),
            snapshot.host_irq_summary,
            snapshot.host_irq_enable,
            snapshot.host_irq_status,
        ),
        Err(error) => dev_err!(
            dev,
            "G17P scheduler gate checkpoint={} read failed ({:?})\n",
            checkpoint,
            error,
        ),
    }
}

const fn g17p_rtkit_uat_address(dva: u64) -> u64 {
    dva & G17P_RTKIT_UAT_DVA_MASK
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u32)]
enum G17PProcessor {
    Gfx = 0,
    Gfx1 = 1,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PFirmwareCrashBuffer {
    dva: u64,
    size: usize,
}

impl G17PProcessor {
    const fn name(self) -> &'static str {
        match self {
            Self::Gfx => "GFX",
            Self::Gfx1 => "GFX1",
        }
    }

    const fn firmware_crash_buffer(self) -> G17PFirmwareCrashBuffer {
        match self {
            Self::Gfx => G17PFirmwareCrashBuffer {
                dva: 0x100_01e1_c000,
                size: 0x8000,
            },
            Self::Gfx1 => G17PFirmwareCrashBuffer {
                dva: 0x100_01f5_0000,
                size: 0x8000,
            },
        }
    }
}

const fn bounded_crashlog_size(size: usize) -> usize {
    if size < G17P_CRASHLOG_MAX_BYTES {
        size
    } else {
        G17P_CRASHLOG_MAX_BYTES
    }
}

const _: () = {
    assert!(bounded_crashlog_size(0) == 0);
    assert!(bounded_crashlog_size(4096) == 4096);
    assert!(bounded_crashlog_size(G17P_CRASHLOG_MAX_BYTES + 1) == G17P_CRASHLOG_MAX_BYTES);
    let gfx = G17PProcessor::Gfx.firmware_crash_buffer();
    let gfx1 = G17PProcessor::Gfx1.firmware_crash_buffer();
    assert!(gfx.dva == 0x100_01e1_c000 && gfx1.dva == 0x100_01f5_0000);
    assert!(gfx.dva == crate::g17_adt_j700::J700_GFX_DATA.base + 0xa_0000);
    assert!(gfx1.dva == crate::g17_adt_j700::J700_GFX1_DATA.base + 0xa_0000);
    assert!(g17p_rtkit_uat_address(gfx.dva) == 0x01e1_c000);
    assert!(g17p_rtkit_uat_address(gfx1.dva) == 0x01f5_0000);
    assert!(gfx.size == 0x8000 && gfx1.size == 0x8000);
    assert!(gfx.dva & (mmu::UAT_PGSZ as u64 - 1) == 0);
    assert!(gfx1.dva & (mmu::UAT_PGSZ as u64 - 1) == 0);
    assert!(gfx.dva + gfx.size as u64 <= 1u64 << 42);
    assert!(gfx1.dva + gfx1.size as u64 <= 1u64 << 42);
};

/// Load-time boundary for short, independent hardware experiments.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PTestStage {
    Control = 0,
    Qid4 = 1,
    ComputeUapi = 2,
}

impl G17PTestStage {
    fn from_parameter(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Control),
            1 => Ok(Self::Qid4),
            2 => Ok(Self::ComputeUapi),
            _ => Err(EINVAL),
        }
    }

    const fn configures_qid4(self) -> bool {
        !matches!(self, Self::Control)
    }

    /// Only the explicit QID4 diagnostic prepares a queue without a submit.
    /// Normal render/compute UAPI sessions allocate it on first compute use.
    const fn prepares_qid4_at_startup(self) -> bool {
        matches!(self, Self::Qid4)
    }

    const fn enables_compute_uapi(self) -> bool {
        matches!(self, Self::ComputeUapi)
    }

    const fn name(self) -> &'static str {
        match self {
            Self::Control => "control",
            Self::Qid4 => "QID4",
            Self::ComputeUapi => "compute UAPI",
        }
    }
}

/// Per-role callback state retained until the corresponding RTKit client is
/// dropped. The callback only publishes atomics; probe owns every mutation of
/// manager and queue state.
struct G17PRoleRtkitState {
    dev: ARef<AsahiDevice>,
    processor: G17PProcessor,
    firmware_contexts: mmu::FirmwareContextTableReader,
    #[cfg(CONFIG_DEV_COREDUMP)]
    crash_capture: Arc<Mutex<G17PCrashCaptureState>>,
    acked: AtomicBool,
    crashed: AtomicBool,
    runtime_events: AtomicU32,
    firmware_event_notifications: AtomicU32,
    firmware_event_notifications_total: AtomicU32,
    event_worker_armed: AtomicBool,
    submission_phase: AtomicU32,
}

impl G17PRoleRtkitState {
    fn new(
        dev: ARef<AsahiDevice>,
        processor: G17PProcessor,
        firmware_contexts: mmu::FirmwareContextTableReader,
        #[cfg(CONFIG_DEV_COREDUMP)] crash_capture: Arc<Mutex<G17PCrashCaptureState>>,
    ) -> Self {
        Self {
            dev,
            processor,
            firmware_contexts,
            #[cfg(CONFIG_DEV_COREDUMP)]
            crash_capture,
            acked: AtomicBool::new(false),
            crashed: AtomicBool::new(false),
            runtime_events: AtomicU32::new(0),
            firmware_event_notifications: AtomicU32::new(0),
            firmware_event_notifications_total: AtomicU32::new(0),
            event_worker_armed: AtomicBool::new(false),
            submission_phase: AtomicU32::new(g17_manager::G17P_SUBMIT_PHASE_IDLE),
        }
    }
}

#[cfg(CONFIG_DEV_COREDUMP)]
struct G17PCrashRecord {
    processor: G17PProcessor,
    original_size: usize,
    crashlog: KVVec<u8>,
}

#[cfg(CONFIG_DEV_COREDUMP)]
struct G17PCrashCaptureState {
    records: [Option<G17PCrashRecord>; 2],
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn new_g17p_crash_capture() -> Result<Arc<Mutex<G17PCrashCaptureState>>> {
    Arc::pin_init(
        new_mutex!(
            G17PCrashCaptureState {
                records: [None, None],
            },
            "G17PCrashCaptureState"
        ),
        GFP_KERNEL,
    )
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn crash_qword(raw: &[u8], index: usize) -> u64 {
    let offset = index * 8;
    u64::from_le_bytes([
        raw[offset],
        raw[offset + 1],
        raw[offset + 2],
        raw[offset + 3],
        raw[offset + 4],
        raw[offset + 5],
        raw[offset + 6],
        raw[offset + 7],
    ])
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn crash_u32(raw: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(raw.get(offset..offset + 4)?.try_into().ok()?))
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn crash_u64(raw: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_le_bytes(raw.get(offset..offset + 8)?.try_into().ok()?))
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn crash_fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
    ((a as u32) << 24) | ((b as u32) << 16) | ((c as u32) << 8) | d as u32
}

#[cfg(CONFIG_DEV_COREDUMP)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PCrashRegisters {
    pc: u64,
    far: u64,
    esr: u64,
    sp: u64,
    psr: u64,
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn g17p_crash_registers(raw: &[u8]) -> Option<G17PCrashRegisters> {
    let total = usize::try_from(crash_u32(raw, 8)?).ok()?.min(raw.len());
    let mut offset = 0x20usize;
    for _ in 0..64 {
        let kind = crash_u32(raw, offset)?;
        let size = usize::try_from(crash_u32(raw, offset + 12)?).ok()?;
        if kind == crash_fourcc(b'C', b'L', b'H', b'E') {
            return None;
        }
        if size < 16 || offset.checked_add(size)? > total {
            return None;
        }
        if kind == crash_fourcc(b'C', b'r', b'g', b'8') {
            let body = offset + 16;
            if size < 16 + 0x350 {
                return None;
            }
            return Some(G17PCrashRegisters {
                sp: crash_u64(raw, body + 0x100)?,
                pc: crash_u64(raw, body + 0x108)?,
                psr: crash_u64(raw, body + 0x110)?,
                far: crash_u64(raw, body + 0x330)?,
                esr: crash_u64(raw, body + 0x340)?,
            });
        }
        offset += size;
    }
    None
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn log_g17p_crash_sections(state: &G17PRoleRtkitState, raw: &[u8]) {
    let Some(header) = crash_u32(raw, 0) else {
        return;
    };
    let Some(total) = crash_u32(raw, 8).and_then(|value| usize::try_from(value).ok()) else {
        return;
    };
    if header != crash_fourcc(b'C', b'L', b'H', b'E') || total > raw.len() {
        dev_err!(
            state.dev.as_ref(),
            "G17P {} crashlog header invalid: fourcc={:#x} size={:#x}/{:#x}\n",
            state.processor.name(),
            header,
            total,
            raw.len()
        );
        return;
    }
    if let Some(regs) = g17p_crash_registers(raw) {
        dev_err!(
            state.dev.as_ref(),
            "G17P {} firmware exception: pc={:#x} far={:#x} esr={:#x} sp={:#x} psr={:#x}\n",
            state.processor.name(),
            regs.pc,
            regs.far,
            regs.esr,
            regs.sp,
            regs.psr
        );
    }

    let mut offset = 0x20usize;
    for _ in 0..64 {
        let Some(kind) = crash_u32(raw, offset) else {
            break;
        };
        let Some(size) = crash_u32(raw, offset + 12)
            .and_then(|value| usize::try_from(value).ok())
        else {
            break;
        };
        if kind == crash_fourcc(b'C', b'L', b'H', b'E') {
            break;
        }
        let Some(end) = offset.checked_add(size) else {
            break;
        };
        if size < 16 || end > total {
            break;
        }
        if kind == crash_fourcc(b'C', b's', b't', b'r') && size >= 20 {
            let body = offset + 16;
            let id = crash_u32(raw, body).unwrap_or(0);
            let bytes = &raw[body + 4..end];
            let length = bytes.iter().position(|byte| *byte == 0).unwrap_or(bytes.len());
            let length = length.min(192);
            if let Ok(message) = core::str::from_utf8(&bytes[..length]) {
                dev_err!(
                    state.dev.as_ref(),
                    "G17P {} firmware message {}: {}\n",
                    state.processor.name(),
                    id,
                    message
                );
            }
        }
        offset = end;
    }
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn log_crash_buffer_observation(
    state: &G17PRoleRtkitState,
    source: &str,
    raw: &[u8],
    sentinel: u8,
) {
    let sentinel_qword = u64::from_le_bytes([sentinel; 8]);
    let mut first_changed = None;
    let mut first_nonzero = None;
    for index in 0..raw.len() / 8 {
        let value = crash_qword(raw, index);
        if first_changed.is_none() && value != sentinel_qword {
            first_changed = Some((index * 8, value));
        }
        if first_nonzero.is_none() && value != 0 {
            first_nonzero = Some((index * 8, value));
        }
        if first_changed.is_some() && first_nonzero.is_some() {
            break;
        }
    }
    let changed = first_changed.unwrap_or((raw.len(), sentinel_qword));
    let nonzero = first_nonzero.unwrap_or((raw.len(), 0));
    dev_info!(
        state.dev.as_ref(),
        "G17P {} crashlog {} sentinel {:#x}: changed={} off={:#x} value={:#x}; nonzero={} off={:#x} value={:#x}\n",
        state.processor.name(),
        source,
        sentinel_qword,
        first_changed.is_some(),
        changed.0,
        changed.1,
        first_nonzero.is_some(),
        nonzero.0,
        nonzero.1
    );
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn publish_g17p_crashlogs(
    dev: &kernel::device::Device,
    records: &[G17PCrashRecord],
) -> Result {
    let mut crashdump = crate::crashdump::CrashDumpBuilder::new(KVVec::new())?;
    for record in records {
        crashdump.add_rtkit_crash_info(
            record.processor as u32,
            record.processor.firmware_crash_buffer().dva,
            record.original_size,
            record.crashlog.len(),
        )?;
        crashdump.add_crashlog(&record.crashlog)?;
    }
    let crashdump = KBox::new(crashdump.finalize()?, GFP_KERNEL)?;
    devcoredump::dev_coredump(
        dev,
        &crate::THIS_MODULE,
        crashdump,
        GFP_KERNEL,
        msecs_to_jiffies(60 * 60 * 1000),
    );
    Ok(())
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn retain_g17p_crashlog(
    state: &G17PRoleRtkitState,
    original_size: usize,
    crashlog: KVVec<u8>,
) -> Result {
    let record = G17PCrashRecord {
        processor: state.processor,
        original_size,
        crashlog,
    };
    publish_g17p_crashlogs(state.dev.as_ref(), core::slice::from_ref(&record))?;
    let mut capture = state.crash_capture.lock();
    capture.records[state.processor as usize] = Some(record);
    dev_info!(
        state.dev.as_ref(),
        "G17P {} crashlog published and retained\n",
        state.processor.name()
    );
    Ok(())
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn retain_g17p_crashlog_slice(
    state: &G17PRoleRtkitState,
    original_size: usize,
    crashlog: &[u8],
) -> Result {
    log_g17p_crash_sections(state, crashlog);
    let mut retained = KVVec::new();
    retained.extend_from_slice(crashlog, GFP_KERNEL)?;
    retain_g17p_crashlog(state, original_size, retained)
}

#[cfg(CONFIG_DEV_COREDUMP)]
fn capture_g17p_firmware_crash_dva(state: &G17PRoleRtkitState) -> Result {
    let buffer = state.processor.firmware_crash_buffer();
    let captured_size = bounded_crashlog_size(buffer.size);
    let physical = crate::pgtable::UatPageTable::copy_firmware_physical_range(
        buffer.dva,
        captured_size,
    )?;
    log_crash_buffer_observation(state, "firmware-physical", &physical, 0);
    log_g17p_crash_sections(state, &physical);
    dev_info!(
        state.dev.as_ref(),
        "G17P {} crashlog copied from firmware data PA {:#x} ({} bytes)\n",
        state.processor.name(),
        buffer.dva,
        physical.len()
    );
    retain_g17p_crashlog(state, buffer.size, physical)
}

/// Active G17P boot does not ask Rust to back system-endpoint buffers. The
/// generic RTKit core uses its coherent-DMA fallback when these callbacks are
/// absent.
struct G17PNoRtkitBuffer;

impl rtkit::Buffer for G17PNoRtkitBuffer {
    fn iova(&self) -> Result<usize> {
        Err(ENOTSUPP)
    }

    fn buf(&mut self) -> Result<IoSysMapRef<'_, u8>> {
        Err(ENOTSUPP)
    }
}

/// Bound ingress diagnostics independently of the pending service counter.
const fn g17p_notification_log_due(total: u32) -> bool {
    total != 0 && (total <= 4 || total.is_power_of_two())
}

struct G17PRtkitOps;

#[vtable]
impl rtkit::Operations for G17PRtkitOps {
    type Data = Arc<G17PRoleRtkitState>;
    type Buffer = G17PNoRtkitBuffer;

    fn recv_message(
        data: <Self::Data as kernel::types::ForeignOwnable>::Borrowed<'_>,
        endpoint: u8,
        message: u64,
    ) {
        if endpoint == g17_rtkit::ENDPOINT_GFX_MESSAGES
            && (message >> 48) as u16 == INITDATA_ACK_TYPE
        {
            data.acked.store(true, Ordering::Release);
        } else if g17_completion::decode_g17p_firmware_event_notification(endpoint, message).is_ok()
        {
            // This callback records ingress and the current publication phase.
            // The serialized runtime owner drains and dispatches the 0x48-byte
            // ring; the IRQ callback never mutates its consumer or queue state.
            let count = data
                .firmware_event_notifications
                .fetch_add(1, Ordering::Relaxed)
                .wrapping_add(1);
            // The pending count is swapped to zero on every drain. Using it
            // as the log budget prints every IRQ during a serviced storm.
            // Keep ingress/dispatch untouched, but budget diagnostics over
            // the complete RTKit role lifetime instead of each drain batch.
            let total = data
                .firmware_event_notifications_total
                .fetch_add(1, Ordering::Relaxed)
                .wrapping_add(1);
            if g17p_notification_log_due(total) {
                dev_info!(
                    data.dev.as_ref(),
                    "G17P firmware-event notification role={} pending={} total={} submit-phase={}\n",
                    data.processor.name(),
                    count,
                    total,
                    data.submission_phase.load(Ordering::Acquire)
                );
            }
            // Queue on a notification from either role. The firmware raises
            // these on GFX1 for a compute submission targeting target1, and the
            // worker reads a single fixed primary-side resource, so gating the
            // queue on the primary role dropped the first submission's records.
            //
            // The original wording here was right and a later edit of mine was
            // wrong: the worker DOES drain channel 13.
            // `read_primary_firmware_event` walks it through
            // `PRIMARY_FIRMWARE_EVENT_CHANNEL_INDEX = 13` at the 0x48 stride.
            // `drain_report_ring` is a read-only WITNESS of that same ring, to
            // show what the worker has or has not consumed; it is not a second
            // consumer and must never advance the cursor by default.
            if data.event_worker_armed.load(Ordering::Acquire) {
                crate::driver::queue_g17p_firmware_event_worker(data.dev.clone());
            }
        } else if endpoint == g17_rtkit::ENDPOINT_GFX_INTERRUPTS {
            data.runtime_events.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn recv_message_early(
        data: <Self::Data as kernel::types::ForeignOwnable>::Borrowed<'_>,
        endpoint: u8,
        message: u64,
    ) -> bool {
        match (endpoint, (message >> 48) as u16) {
            (g17_rtkit::ENDPOINT_GFX_MESSAGES, INITDATA_ACK_TYPE) => {
                data.acked.store(true, Ordering::Release);
                true
            }
            _ => false,
        }
    }

    fn crashed(
        data: <Self::Data as kernel::types::ForeignOwnable>::Borrowed<'_>,
        crashlog: Option<&[u8]>,
    ) {
        data.crashed.store(true, Ordering::Release);
        dev_err!(
            data.dev.as_ref(),
            "G17P {} RTKit crash callback invoked; crashlog-present={}\n",
            data.processor.name(),
            crashlog.is_some()
        );
        #[cfg(CONFIG_DEV_COREDUMP)]
        let firmware_owned = crashlog.is_none();
        #[cfg(CONFIG_DEV_COREDUMP)]
        let capture = match crashlog {
            Some(crashlog) => {
                let captured_size = bounded_crashlog_size(crashlog.len());
                retain_g17p_crashlog_slice(
                    &data,
                    crashlog.len(),
                    &crashlog[..captured_size],
                )
            }
            None => capture_g17p_firmware_crash_dva(&data),
        };
        #[cfg(CONFIG_DEV_COREDUMP)]
        if let Err(error) = capture {
            if firmware_owned {
                let buffer = data.processor.firmware_crash_buffer();
                let live_root = data
                    .firmware_contexts
                    .context0_root()
                    .map(|root| root.ttb())
                    .unwrap_or(0);
                dev_err!(
                    data.dev.as_ref(),
                    "G17P {} crashlog copy from DVA {:#x} via live context-0 root {:#x} VA {:#x} size {:#x} failed ({:?})\n",
                    data.processor.name(),
                    buffer.dva,
                    live_root,
                    g17p_rtkit_uat_address(buffer.dva),
                    buffer.size,
                    error
                );
            } else {
                dev_err!(
                    data.dev.as_ref(),
                    "G17P {} crashlog coredump failed ({:?})\n",
                    data.processor.name(),
                    error
                );
            }
        }
        #[cfg(not(CONFIG_DEV_COREDUMP))]
        let _ = crashlog;
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct ExpectedProvider {
    role: G17PAscwrapV6Role,
    wrapper: (u64, u64),
    iop_vbar: (u64, u64),
}

const GFX_PROVIDER: ExpectedProvider = ExpectedProvider {
    role: G17PAscwrapV6Role::Gfx,
    wrapper: (0x4826_00000, 0x88000),
    iop_vbar: (0x4820_50000, 0x8),
};

const GFX1_PROVIDER: ExpectedProvider = ExpectedProvider {
    role: G17PAscwrapV6Role::Gfx1,
    wrapper: (0x482e_00000, 0x88000),
    iop_vbar: (0x4828_50000, 0x8),
};

fn validate_provider(
    provider: &G17PMailboxProvider,
    expected: ExpectedProvider,
) -> Result<G17PAscwrapV6Lifecycle> {
    let lifecycle = provider.lifecycle()?;
    if lifecycle.role != expected.role
        || lifecycle.wrapper != expected.wrapper
        || lifecycle.iop_vbar != expected.iop_vbar
    {
        return Err(EINVAL);
    }

    Ok(lifecycle)
}

impl g17_lifecycle::AscCpuLifecycle for G17PMailboxProvider {
    type Error = Error;

    fn start_cpu(&self) -> Result {
        G17PMailboxProvider::start_cpu(self)
    }

    fn stop_cpu(&self) -> Result {
        G17PMailboxProvider::stop_cpu(self)
    }
}

struct G17PBootedSession {
    cpus: g17_lifecycle::StartedCpuPair<G17PMailboxProvider>,
    gfx_state: Arc<G17PRoleRtkitState>,
    gfx_rtkit: rtkit::RtKit<G17PRtkitOps>,
    gfx1_state: Arc<G17PRoleRtkitState>,
    gfx1_rtkit: rtkit::RtKit<G17PRtkitOps>,
    queue: Option<g17_manager::G17PSksmQueue>,
    compute_uapi_enabled: bool,
    queue_setup_pending: bool,
}

fn wait_initdata_ack(state: &G17PRoleRtkitState) -> Result {
    for _ in 0..INITDATA_ACK_POLLS {
        if state.acked.load(Ordering::Acquire) {
            return Ok(());
        }
        if state.crashed.load(Ordering::Acquire) {
            return Err(EIO);
        }
        fsleep(Delta::from_millis(10));
    }
    Err(ETIMEDOUT)
}

#[allow(clippy::too_many_arguments)]
fn finish_boot_session(
    pdev: &platform::Device,
    _drm: &ARef<AsahiDevice>,
    manager: &mut g17_manager::G17PManagerConstruction,
    test_stage: G17PTestStage,
    cpus: g17_lifecycle::StartedCpuPair<G17PMailboxProvider>,
    gfx_state: Arc<G17PRoleRtkitState>,
    gfx_rtkit: rtkit::RtKit<G17PRtkitOps>,
    gfx1_state: Arc<G17PRoleRtkitState>,
    gfx1_rtkit: rtkit::RtKit<G17PRtkitOps>,
) -> Result<G17PBootedSession> {
    let mut counters = manager.control_counters()?;
    for _ in 0..CONTROL_COUNTER_POLLS {
        if counters.opening_retired() {
            break;
        }
        if gfx_state.crashed.load(Ordering::Acquire)
            || gfx1_state.crashed.load(Ordering::Acquire)
        {
            return Err(EIO);
        }
        fsleep(Delta::from_millis(1));
        counters = manager.control_counters()?;
    }
    if !counters.opening_retired() {
        dev_err!(
            pdev.as_ref(),
            "G17P control: counters stopped at {:?}/{:?}, expected {:?}/{:?}\n",
            counters.primary,
            counters.secondary,
            [g17_initdata::CONTROL_OPENING_PRIMARY_PRODUCER; 3],
            [g17_initdata::CONTROL_OPENING_SECONDARY_PRODUCER; 3],
        );
        return Err(ETIMEDOUT);
    }
    dev_info!(
        pdev.as_ref(),
        "G17P control: counters retired by the single native post-initdata 0x89; no duplicate start message sent\n"
    );
    log_g17p_firmware_recovery_handshake(
        pdev.as_ref(),
        manager,
        "native-opening-retired-baseline",
    );
    log_g17p_firmware_recovery_handshake(
        pdev.as_ref(),
        manager,
        "native-opening-recovery-deferred-to-runtime-worker",
    );
    log_g17p_fault_report(
        pdev.as_ref(),
        manager,
        "native-opening-recovery-deferred-to-runtime-worker",
    );

    if !g17_submission::G17P_NATIVE_PRE_USER_EP21_MESSAGES.is_empty() {
        return Err(EINVAL);
    }
    manager.validate_native_pre_user_roots()?;
    if !test_stage.configures_qid4() {
        dev_info!(
            pdev.as_ref(),
            "G17P session: native control stage complete; no pre-user EP21 work/control message; QID4 remains untouched\n"
        );
        return Ok(G17PBootedSession {
            cpus,
            gfx_state,
            gfx_rtkit,
            gfx1_state,
            gfx1_rtkit,
            queue: None,
            compute_uapi_enabled: false,
            queue_setup_pending: false,
        });
    }

    if test_stage.prepares_qid4_at_startup() {
        dev_info!(
            pdev.as_ref(),
            "G17P session: explicit QID4 diagnostic defers queue memory preparation to serialized runtime work\n"
        );
    } else {
        dev_info!(
            pdev.as_ref(),
            "G17P session: render/compute UAPI ready; QID4 allocation deferred until first compute submit\n"
        );
    }
    dev_info!(
        pdev.as_ref(),
        "G17P diagnostic: j700-dual-initdata-publication-v1 armed\n"
    );
    Ok(G17PBootedSession {
        cpus,
        gfx_state,
        gfx_rtkit,
        gfx1_state,
        gfx1_rtkit,
        queue: None,
        compute_uapi_enabled: test_stage.enables_compute_uapi(),
        queue_setup_pending: test_stage.prepares_qid4_at_startup(),
    })
}

fn boot_session(
    pdev: &platform::Device,
    drm: &ARef<AsahiDevice>,
    registers: &regs::Resources,
    manager: &mut g17_manager::G17PManagerConstruction,
    test_stage: G17PTestStage,
) -> Result<G17PBootedSession> {
    let gfx = G17PMailboxProvider::get(pdev.as_ref(), 0)?;
    let gfx_lifecycle = validate_provider(&gfx, GFX_PROVIDER)?;
    let gfx1 = G17PMailboxProvider::get(pdev.as_ref(), 1)?;
    let gfx1_lifecycle = validate_provider(&gfx1, GFX1_PROVIDER)?;
    let gfx_gate = gfx.require_safe_cpu_lifecycle();
    let gfx1_gate = gfx1.require_safe_cpu_lifecycle();
    let transaction_gate =
        g17_lifecycle::admit_cpu_start(gfx_lifecycle.missing, gfx1_lifecycle.missing);
    if gfx_gate.is_err() || gfx1_gate.is_err() || transaction_gate.is_err() {
        return Err(ENOTSUPP);
    }
    dev_info!(pdev.as_ref(), "G17P session: providers ready\n");

    let firmware_contexts = manager.uat().firmware_context_table_reader();
    #[cfg(CONFIG_DEV_COREDUMP)]
    let crash_capture = new_g17p_crash_capture()?;
    let gfx_state = Arc::new(
        G17PRoleRtkitState::new(
            drm.clone(),
            G17PProcessor::Gfx,
            firmware_contexts.clone(),
            #[cfg(CONFIG_DEV_COREDUMP)]
            crash_capture.clone(),
        ),
        GFP_KERNEL,
    )?;
    let gfx1_state = Arc::new(
        G17PRoleRtkitState::new(
            drm.clone(),
            G17PProcessor::Gfx1,
            firmware_contexts,
            #[cfg(CONFIG_DEV_COREDUMP)]
            crash_capture,
        ),
        GFP_KERNEL,
    )?;
    let mut gfx_rtkit =
        rtkit::RtKit::<G17PRtkitOps>::new_g17p(drm.as_ref(), None, 0, gfx_state.clone())?;
    let mut gfx1_rtkit =
        rtkit::RtKit::<G17PRtkitOps>::new_g17p(drm.as_ref(), None, 1, gfx1_state.clone())?;
    dev_info!(pdev.as_ref(), "G17P session: both RTKit receivers armed\n");

    registers.validate_t8140_axi_transition()?;
    dev_info!(pdev.as_ref(), "G17P session: provider AXI transition validated\n");
    let trace_mask = *module_parameters::g17p_fw_trace.value();
    if trace_mask != 0 {
        if let Err(error) = manager.arm_firmware_trace_classes(trace_mask) {
            dev_warn!(
                pdev.as_ref(),
                "G17P firmware trace arm failed ({:?})\n",
                error
            );
        }
    }
    let handoff = manager.handoff();
    let gfx_init =
        g17_rtkit::encode_initdata_doorbell(handoff.primary.instance.root).map_err(|_| EINVAL)?;
    let gfx1_init =
        g17_rtkit::encode_initdata_doorbell(handoff.secondary.instance.root).map_err(|_| EINVAL)?;
    // Marker + timing on the two steps a session REBUILD can hang in.
    //
    // `apple_rtkit_boot` performs two `apple_rtkit_wait_for_completion` waits,
    // each `msecs_to_jiffies(1000)`: first `epmap_completion` (the IOP's
    // HELLO/EPMAP), then `iop_pwr_ack_completion`. So ONE timed-out RTKit boot
    // costs almost exactly 1.00s -- which is what a 1.02s failed rebuild is
    // made of, and NOT a `wait_initdata_ack` timeout, whose budget is
    // INITDATA_ACK_POLLS (200) x 10ms = 2.0s. These markers say which step it
    // actually was, and on which role, without inferring it from arithmetic.
    let cpus = g17_lifecycle::StartedCpuPair::start_staged(gfx, gfx1, || {
        dev_info!(pdev.as_ref(), "G17P session: GFX processor started\n");
        let started = Instant::<Monotonic>::now();
        let result = Pin::new(&mut gfx_rtkit).boot();
        dev_info!(
            pdev.as_ref(),
            "G17PMARK rtkit-boot role=gfx ok={} took={}us -- ~1000000us means apple_rtkit_boot timed out on the IOP HELLO/EPMAP, i.e. the restarted ASC never re-ran its RTKit handshake\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
        dev_info!(pdev.as_ref(), "G17P session: GFX RTKit handshake complete\n");
        Ok(())
    })?;
    dev_info!(pdev.as_ref(), "G17P session: GFX1 processor started\n");
    {
        let started = Instant::<Monotonic>::now();
        let result = Pin::new(&mut gfx1_rtkit).boot();
        dev_info!(
            pdev.as_ref(),
            "G17PMARK rtkit-boot role=gfx1 ok={} took={}us\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
    }
    dev_info!(pdev.as_ref(), "G17P session: GFX1 RTKit handshake complete\n");

    let root_snapshot = manager.uat().prepare_t8140_firmware_high_roots()?;
    manager
        .uat()
        .confirm_t8140_primary_high_root(root_snapshot)?;
    manager.uat().mirror_t8140_secondary_high_root()?;
    registers.t8140_pre_initdata_clear()?;

    Pin::new(&mut gfx_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_MESSAGES)?;
    Pin::new(&mut gfx_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_INTERRUPTS)?;
    Pin::new(&mut gfx1_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_MESSAGES)?;
    Pin::new(&mut gfx1_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_INTERRUPTS)?;
    Pin::new(&mut gfx_rtkit).send_message(g17_rtkit::ENDPOINT_GFX_MESSAGES, gfx_init)?;
    Pin::new(&mut gfx1_rtkit).send_message(g17_rtkit::ENDPOINT_GFX_MESSAGES, gfx1_init)?;
    dev_info!(
        pdev.as_ref(),
        "G17P session: primary and secondary initdata published\n"
    );
    {
        let started = Instant::<Monotonic>::now();
        let result = wait_initdata_ack(&gfx_state);
        dev_info!(
            pdev.as_ref(),
            "G17PMARK initdata-ack role=gfx ok={} took={}us (budget 2000000us)\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
    }
    dev_info!(pdev.as_ref(), "G17P session: primary initdata ACK received\n");
    {
        let started = Instant::<Monotonic>::now();
        let result = wait_initdata_ack(&gfx1_state);
        dev_info!(
            pdev.as_ref(),
            "G17PMARK initdata-ack role=gfx1 ok={} took={}us\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
    }
    dev_info!(pdev.as_ref(), "G17P session: secondary initdata ACK received\n");

    probe_g17p_ktrace(pdev.as_ref(), manager, "post-initdata-ack");

    finish_boot_session(
        pdev,
        drm,
        manager,
        test_stage,
        cpus,
        gfx_state,
        gfx_rtkit,
        gfx1_state,
        gfx1_rtkit,
    )
}

#[inline(never)]
fn boot_initial_session(
    pdev: &platform::Device,
    drm: &ARef<AsahiDevice>,
    registers: &regs::Resources,
    soc: &'static hw::agx3::SocConfig,
    num_clusters: u32,
    gpc_perf_state_map: u32,
    gpc_perf_state_map_low: u32,
    gpc_perf_state_control: u32,
    test_stage: G17PTestStage,
) -> Result<(KBox<g17_manager::G17PManagerConstruction>, G17PBootedSession)> {
    let gfx = G17PMailboxProvider::get(pdev.as_ref(), 0)?;
    let gfx_lifecycle = validate_provider(&gfx, GFX_PROVIDER)?;
    let gfx1 = G17PMailboxProvider::get(pdev.as_ref(), 1)?;
    let gfx1_lifecycle = validate_provider(&gfx1, GFX1_PROVIDER)?;
    let gfx_gate = gfx.require_safe_cpu_lifecycle();
    let gfx1_gate = gfx1.require_safe_cpu_lifecycle();
    let transaction_gate =
        g17_lifecycle::admit_cpu_start(gfx_lifecycle.missing, gfx1_lifecycle.missing);
    if gfx_gate.is_err() || gfx1_gate.is_err() || transaction_gate.is_err() {
        return Err(ENOTSUPP);
    }
    dev_info!(pdev.as_ref(), "G17P session: providers ready\n");

    let mut uat = Some(KBox::new(mmu::Uat::new_t8140(drm, true)?, GFP_KERNEL)?);
    let firmware_contexts = uat
        .as_ref()
        .ok_or(EIO)?
        .firmware_context_table_reader();
    #[cfg(CONFIG_DEV_COREDUMP)]
    let crash_capture = new_g17p_crash_capture()?;
    let gfx_state = Arc::new(
        G17PRoleRtkitState::new(
            drm.clone(),
            G17PProcessor::Gfx,
            firmware_contexts.clone(),
            #[cfg(CONFIG_DEV_COREDUMP)]
            crash_capture.clone(),
        ),
        GFP_KERNEL,
    )?;
    let gfx1_state = Arc::new(
        G17PRoleRtkitState::new(
            drm.clone(),
            G17PProcessor::Gfx1,
            firmware_contexts,
            #[cfg(CONFIG_DEV_COREDUMP)]
            crash_capture,
        ),
        GFP_KERNEL,
    )?;
    let mut gfx_rtkit =
        rtkit::RtKit::<G17PRtkitOps>::new_g17p(drm.as_ref(), None, 0, gfx_state.clone())?;
    let mut gfx1_rtkit =
        rtkit::RtKit::<G17PRtkitOps>::new_g17p(drm.as_ref(), None, 1, gfx1_state.clone())?;
    dev_info!(pdev.as_ref(), "G17P session: both RTKit receivers armed\n");

    registers.validate_t8140_axi_transition()?;
    dev_info!(pdev.as_ref(), "G17P session: provider AXI transition validated\n");
    // Marker + timing on the two steps a session REBUILD can hang in.
    //
    // `apple_rtkit_boot` performs two `apple_rtkit_wait_for_completion` waits,
    // each `msecs_to_jiffies(1000)`: first `epmap_completion` (the IOP's
    // HELLO/EPMAP), then `iop_pwr_ack_completion`. So ONE timed-out RTKit boot
    // costs almost exactly 1.00s -- which is what a 1.02s failed rebuild is
    // made of, and NOT a `wait_initdata_ack` timeout, whose budget is
    // INITDATA_ACK_POLLS (200) x 10ms = 2.0s. These markers say which step it
    // actually was, and on which role, without inferring it from arithmetic.
    let cpus = g17_lifecycle::StartedCpuPair::start_staged(gfx, gfx1, || {
        dev_info!(pdev.as_ref(), "G17P session: GFX processor started\n");
        let started = Instant::<Monotonic>::now();
        let result = Pin::new(&mut gfx_rtkit).boot();
        dev_info!(
            pdev.as_ref(),
            "G17PMARK rtkit-boot role=gfx ok={} took={}us -- ~1000000us means apple_rtkit_boot timed out on the IOP HELLO/EPMAP, i.e. the restarted ASC never re-ran its RTKit handshake\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
        dev_info!(pdev.as_ref(), "G17P session: GFX RTKit handshake complete\n");
        Ok(())
    })?;
    dev_info!(pdev.as_ref(), "G17P session: GFX1 processor started\n");
    {
        let started = Instant::<Monotonic>::now();
        let result = Pin::new(&mut gfx1_rtkit).boot();
        dev_info!(
            pdev.as_ref(),
            "G17PMARK rtkit-boot role=gfx1 ok={} took={}us\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
    }
    dev_info!(pdev.as_ref(), "G17P session: GFX1 RTKit handshake complete\n");

    let root_snapshot = uat
        .as_ref()
        .ok_or(EIO)?
        .prepare_t8140_firmware_high_roots()?;
    let mut manager = g17_manager::G17PManagerConstruction::new_with_uat(
        drm,
        soc,
        g17_manager::T8140_G17P_PLATFORM_CONFIG,
        num_clusters,
        gpc_perf_state_map,
        gpc_perf_state_map_low,
        gpc_perf_state_control,
        uat.take().ok_or(EIO)?,
    )?;
    manager
        .uat()
        .confirm_t8140_primary_high_root(root_snapshot)?;
    manager.uat().mirror_t8140_secondary_high_root()?;
    registers.t8140_pre_initdata_clear()?;

    let trace_mask = *module_parameters::g17p_fw_trace.value();
    if trace_mask != 0 {
        if let Err(error) = manager.arm_firmware_trace_classes(trace_mask) {
            dev_warn!(
                pdev.as_ref(),
                "G17P firmware trace arm failed ({:?})\n",
                error
            );
        }
    }
    let handoff = manager.handoff();
    let gfx_init =
        g17_rtkit::encode_initdata_doorbell(handoff.primary.instance.root).map_err(|_| EINVAL)?;
    let gfx1_init = g17_rtkit::encode_initdata_doorbell(handoff.secondary.instance.root)
        .map_err(|_| EINVAL)?;
    Pin::new(&mut gfx_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_MESSAGES)?;
    Pin::new(&mut gfx_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_INTERRUPTS)?;
    Pin::new(&mut gfx_rtkit).send_message(g17_rtkit::ENDPOINT_GFX_MESSAGES, gfx_init)?;
    dev_info!(
        pdev.as_ref(),
        "G17P session: primary and secondary initdata published\n"
    );
    {
        let started = Instant::<Monotonic>::now();
        let result = wait_initdata_ack(&gfx_state);
        dev_info!(
            pdev.as_ref(),
            "G17PMARK initdata-ack role=gfx ok={} took={}us (budget 2000000us)\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
    }
    dev_info!(pdev.as_ref(), "G17P session: primary initdata ACK received\n");

    Pin::new(&mut gfx1_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_MESSAGES)?;
    Pin::new(&mut gfx1_rtkit).start_endpoint(g17_rtkit::ENDPOINT_GFX_INTERRUPTS)?;
    Pin::new(&mut gfx1_rtkit).send_message(g17_rtkit::ENDPOINT_GFX_MESSAGES, gfx1_init)?;
    {
        let started = Instant::<Monotonic>::now();
        let result = wait_initdata_ack(&gfx1_state);
        dev_info!(
            pdev.as_ref(),
            "G17PMARK initdata-ack role=gfx1 ok={} took={}us\n",
            result.is_ok(),
            started.elapsed().as_micros_ceil(),
        );
        result?;
    }
    dev_info!(pdev.as_ref(), "G17P session: secondary initdata ACK received\n");

    {
        let note = g17_rtkit::encode_power_transition_end();
        Pin::new(&mut gfx_rtkit).send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, note)?;
        Pin::new(&mut gfx1_rtkit).send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, note)?;
        dev_info!(
            pdev.as_ref(),
            "G17P session: power-transition-end {:#018x} sent to both roles\n",
            note,
        );
    }

    probe_g17p_ktrace(pdev.as_ref(), &mut manager, "post-initdata-ack");

    let session = finish_boot_session(
        pdev,
        drm,
        &mut manager,
        test_stage,
        cpus,
        gfx_state,
        gfx_rtkit,
        gfx1_state,
        gfx1_rtkit,
    )?;
    Ok((manager, session))
}

/// State retained for the full lifetime of an exact T8140 platform device.
/// Hardware/runtime owner used by the T8140 DRM frontend.
/// One firmware completion, as the firmware actually reports it.
///
/// The G17 firmware signals completion the same way the M1/M2 parts do: with an
/// event "stamp" that advances by `0x100` per completed submission (upstream's
/// `EventValue::next`). Our first submit is answered with `stamp = 0x100`.
#[derive(Debug, Copy, Clone)]
pub(crate) struct G17PComputeCompletion {
    /// The outermost GPU timestamp pair from the KSM completion record.
    pub(crate) timestamps: [u64; 2],
}

/// What a publish returned: either a finished submission (legacy polling wait)
/// or a live handle to wait on with the runtime mutex released.
pub(crate) enum G17PSubmitOutcome {
    Completed([u64; 2]),
    Pending(Arc<G17PCompletionSignal>),
}

/// Interrupt-driven handoff between the firmware-event drain and a submitter.
///
/// The firmware raises an RTKit doorbell on the GFX messages endpoint and
/// publishes a completion record in the event ring; the drain decodes it and
/// wakes the waiter here. Waiting on this must be done with the runtime mutex
/// released -- the drain needs that mutex to run at all, which is exactly why
/// the previous timestamp-polling wait could never observe a completion.
#[pin_data]
pub(crate) struct G17PCompletionSignal {
    #[pin]
    state: Mutex<Option<G17PComputeCompletion>>,
    #[pin]
    cond: CondVar,
}

impl G17PCompletionSignal {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(G17PCompletionSignal {
                state <- new_mutex!(None, "G17PCompletionSignal::state"),
                cond <- new_condvar!("G17PCompletionSignal::cond"),
            }),
            GFP_KERNEL,
        )
    }

    /// Clear any stale completion before publishing new work.
    pub(crate) fn arm(&self) {
        *self.state.lock() = None;
    }

    /// Publish a completion decoded from the firmware event ring and wake the
    /// submitter. Called from the drain, which already holds the runtime mutex.
    pub(crate) fn record(&self, completion: G17PComputeCompletion) {
        *self.state.lock() = Some(completion);
        self.cond.notify_all();
    }

    /// Block until the firmware signals, or the timeout expires.
    pub(crate) fn wait_timeout(&self, millis: u32) -> Option<G17PComputeCompletion> {
        let jiffies = msecs_to_jiffies(millis);
        let mut state = self.state.lock();
        while state.is_none() {
            match self.cond.wait_interruptible_timeout(&mut state, jiffies) {
                CondVarTimeoutResult::Timeout => break,
                CondVarTimeoutResult::Signal { .. } => break,
                CondVarTimeoutResult::Woken { .. } => continue,
            }
        }
        *state
    }
}

pub(crate) struct G17PLiveRuntime {
    pdev: Option<ARef<platform::Device>>,
    drm: Option<ARef<AsahiDevice>>,
    registers: Option<regs::Resources>,
    manager: Option<KBox<g17_manager::G17PManagerConstruction>>,
    cpus: Option<g17_lifecycle::StartedCpuPair<G17PMailboxProvider>>,
    gfx_state: Option<Arc<G17PRoleRtkitState>>,
    gfx_rtkit: Option<rtkit::RtKit<G17PRtkitOps>>,
    gfx1_state: Option<Arc<G17PRoleRtkitState>>,
    gfx1_rtkit: Option<rtkit::RtKit<G17PRtkitOps>>,
    queue: Option<g17_manager::G17PSksmQueue>,
    compute_binding: Option<g17_manager::G17PComputeUserBinding>,
    compute_timestamps: Option<mmu::KernelMapping>,
    completion_indices: g17_completion::QueueIndices,
    render_tracker: Option<g17_manager::G17PPartialOpeningRenderTracker>,
    test_stage: G17PTestStage,
    accepting_submissions: bool,
    queue_setup_pending: bool,
    /// DRM-side id of the queue object that currently owns the single
    /// hardware compute graph (QID 4) and the single bindable user-VM slot.
    ///
    /// `None` means nothing is bound and the next submitter may take it. A
    /// submit from a different id is a *handoff*: the previous owner's
    /// binding is released and the queue graph is recycled before the new
    /// owner's first bind, because `validate_prepublication_compute_queue`
    /// legitimately refuses a queue that has already run.
    compute_owner: Option<u64>,
    /// The owner released the graph (queue_destroy / file close) but the
    /// recycle was deferred to the next submit. Doing it at release time would
    /// mean running the firmware handshake from a dying process's file-release
    /// path; doing it here keeps every firmware-visible step on a submit.
    compute_recycle_pending: bool,
    /// A submission was published and never retired: the doorbell wait timed
    /// out, or the finish step failed. The firmware may still be holding a
    /// kick that names the owner's VM, so the graph must NOT be handed on
    /// cheaply -- the next handoff stops both CPUs first, which is the only
    /// state in which releasing that binding is provably safe.
    compute_abandoned: bool,
    /// Queue graphs a handoff took off the SKSM bridge.
    ///
    /// They are NOT freed: the firmware keeps per-QID state at
    /// `0x127628 + qid * 0x28` that names the entry ring and descriptor pages
    /// of whichever graph was last configured, and no host-visible message
    /// proves it has forgotten them. Retiring instead of freeing means a stale
    /// firmware pointer can only reach memory nothing else owns. They are
    /// released together once both CPUs stop, in `release_runtime_objects`.
    retired_queues: KVec<g17_manager::G17PSksmQueue>,
    async_compute_queues: KVec<G17PAsyncComputeQueue>,
    async_visibility_pending: bool,
    /// Bindings of clients whose submission was published and never seen to
    /// retire.
    ///
    /// They are NOT dropped: the firmware may still hold that kick, and the
    /// kick names this client's page tables and the GEM objects mapped into
    /// them. Each entry keeps those alive while having already handed UAT slot
    /// 1 back, so the next client binds immediately. Released together once
    /// both CPUs stop, in `release_runtime_objects`.
    quarantined_bindings: KVec<g17_manager::G17PQuarantinedComputeBinding>,
    /// Crashed-session rebuilds attempted, bounding `recover_crashed_session_once`.
    crash_recoveries: u32,
    /// The firmware retired a submission the GPU demonstrably never executed
    /// (`end == start`), so the GPU core power / KSM pause gate is closed and
    /// no host lever this driver has is known to re-open it. The next handoff
    /// must rebuild the firmware session rather than hand the retained graph
    /// on, because a rebuild is the only state in which execution has ever
    /// been observed to come back.
    compute_gate_suspect: bool,
    completion_signal: Arc<G17PCompletionSignal>,
    /// Set by a publish that returned `Pending`; consumed by the finish step
    /// once the firmware has signalled.
    pending_compute: Option<(bool, mmu::KernelMapping)>,
}

impl G17PLiveRuntime {
    pub(crate) fn arm_primary_firmware_event_worker(&self) -> Result {
        let state = self.gfx_state.as_ref().ok_or(ENODEV)?;
        state.event_worker_armed.store(true, Ordering::Release);
        // Arm the secondary role too. `recv_message` gates queueing the worker
        // on the flag of whichever role received the notification, and the
        // firmware raises compute completions on GFX1 as well as GFX -- arming
        // only the primary silently dropped every GFX1 notification, which with
        // the doorbell-driven completion path would mean the waiter is never
        // woken at all.
        if let Some(secondary) = self.gfx1_state.as_ref() {
            secondary.event_worker_armed.store(true, Ordering::Release);
        }
        if self.primary_firmware_event_pending()
        {
            crate::driver::queue_g17p_firmware_event_worker(state.dev.clone());
        }
        Ok(())
    }

    /// The interrupt-driven completion handoff for this runtime.
    ///
    /// Cloned out before the caller drops the runtime mutex, so the wait can
    /// happen with that mutex released.
    pub(crate) fn completion_signal(&self) -> Arc<G17PCompletionSignal> {
        self.completion_signal.clone()
    }

    pub(crate) fn primary_firmware_event_pending(&self) -> bool {
        if self.is_crashed() {
            return false;
        }
        self.gfx_state.as_ref().is_some_and(|state| {
            state.firmware_event_notifications.load(Ordering::Acquire) != 0
        }) || self.gfx1_state.as_ref().is_some_and(|state| {
            state.firmware_event_notifications.load(Ordering::Acquire) != 0
        }) || self.queue_setup_pending || self.async_visibility_pending
    }

    pub(crate) fn primary_recovery_pending(&self) -> bool {
        self.manager
            .as_ref()
            .is_some_and(|manager| manager.pending_primary_recovery().is_some())
    }

    /// Acquire this pair's FList lease before any render doorbell. Only a
    /// pending cold grow publishes 0x20 and waits for type 13; retained pairs
    /// still acquire command references but return ready without a grow.
    fn publish_render_usc_freelist_request(&mut self) -> Result {
        if *module_parameters::g17p_render_usc_freelist.value() == 0 {
            return Ok(());
        }
        let Some(publication) = self
            .manager
            .as_mut()
            .ok_or(ENODEV)?
            .publish_render_usc_freelist_request()?
        else {
            return Ok(());
        };
        fence(Ordering::SeqCst);
        let announcement = g17_rtkit::encode_primary_device_control();
        dev_info!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P render USC FList: staged opcode={:#x} stamp={} slot={} record={:#x} producer {}->{} consumer={}; announcing {:#x}\n",
            publication.opcode,
            publication.arg,
            publication.slot,
            publication.record_address,
            publication.producer_before,
            publication.producer_after,
            publication.consumer_before,
            announcement,
        );
        Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
            .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, announcement)?;

        let target = publication.producer_after;
        let mut counters = self.manager.as_mut().ok_or(ENODEV)?.control_counters()?;
        let mut control_retired = false;
        for poll in 0..CONTROL_COUNTER_POLLS {
            if counters.primary == [target; 3] {
                dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P render USC FList: control record retired before work after {}ms counters={:?}\n",
                    poll,
                    counters.primary,
                );
                control_retired = true;
                break;
            }
            if self.is_crashed() {
                return Err(EIO);
            }
            fsleep(Delta::from_millis(1));
            counters = self.manager.as_mut().ok_or(ENODEV)?.control_counters()?;
        }
        if !control_retired {
            dev_err!(
                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                "G17P render USC FList: control retirement timed out target={} counters={:?}\n",
                target,
                counters.primary,
            );
            return Err(ETIMEDOUT);
        }

        // B1 marks c083 +0x44 busy while opcode 0x20 drives UMA. Registering
        // render qids against that busy owner suspends them with reason 4;
        // the verbose firmware trace shows the later type-13 event resuming
        // qids0/1 immediately before qid1 is NOP-retired. Service that event
        // now, while this thread owns the runtime mutex, and do not publish a
        // render doorbell until the retained lifecycle reaches Matched.
        const UMA_GROW_POLLS: usize = 200;
        let mut serviced = 0u32;
        for poll in 0..UMA_GROW_POLLS {
            if self
                .manager
                .as_ref()
                .is_some_and(|manager| manager.render_usc_freelist_grow_matched())
            {
                dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P render USC FList: type13 grow completed before work after {}ms serviced={}\n",
                    poll,
                    serviced,
                );
                return Ok(());
            }
            if self.is_crashed() {
                return Err(EIO);
            }
            serviced = serviced.saturating_add(self.service_firmware_events_inline()?);
            fsleep(Delta::from_millis(1));
        }
        dev_err!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P render USC FList: type13 grow did not complete before work (serviced={})\n",
            serviced,
        );
        Err(ETIMEDOUT)
    }

    fn service_render_usc_freelist_release(
        &mut self,
        release: g17_manager::G17PBootstrapUmaRelease,
    ) -> Result {
        let publication = self
            .manager
            .as_mut()
            .ok_or(ENODEV)?
            .publish_render_usc_freelist_release(release)?;
        fence(Ordering::SeqCst);
        let announcement = g17_rtkit::encode_primary_device_control();
        dev_info!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P render USC FList: staged release opcode={:#x} sequence={:#x} ID={} slot={} producer {}->{}; announcing {:#x}\n",
            publication.opcode,
            release.sequence,
            release.hardware_buffer_id,
            publication.slot,
            publication.producer_before,
            publication.producer_after,
            announcement,
        );
        Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
            .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, announcement)?;
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .mark_render_usc_freelist_release_sent(publication.producer_after)?;

        for _ in 0..20 {
            fence(Ordering::SeqCst);
            if self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .complete_render_usc_freelist_release_if_consumed(
                    publication.producer_after,
                )?
            {
                dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P render USC FList: release consumed at producer {}\n",
                    publication.producer_after,
                );
                return Ok(());
            }
            fsleep(Delta::from_micros(50));
        }
        dev_warn!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P render USC FList: release remains asynchronous at producer {}; retaining owner for later settlement\n",
            publication.producer_after,
        );
        Ok(())
    }

    /// A release that missed the short completion-path observation window is
    /// ordinary control-ring backpressure, not a corrupt retained owner. Wait
    /// for the exact published producer before acquiring the next command's
    /// FList references. The same bounded 100ms retirement contract is used by
    /// the synchronous opcode-0x20 path above.
    fn settle_render_usc_freelist_release_before_submission(&mut self) -> Result {
        let Some(target) = self
            .manager
            .as_ref()
            .and_then(|manager| manager.pending_render_usc_freelist_release_producer())
        else {
            return Ok(());
        };

        for poll in 0..CONTROL_COUNTER_POLLS {
            fence(Ordering::SeqCst);
            if self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .complete_render_usc_freelist_release_if_consumed(target)?
            {
                dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P render USC FList: settled pending release producer {} before next submission after {}ms\n",
                    target,
                    poll,
                );
                return Ok(());
            }
            if self.is_crashed() {
                return Err(EIO);
            }
            fsleep(Delta::from_millis(1));
        }

        dev_err!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P render USC FList: pending release producer {} did not retire before next submission\n",
            target,
        );
        Err(ETIMEDOUT)
    }

    pub(crate) fn service_primary_firmware_events(&mut self) -> Result {
        if self.is_crashed() {
            return Ok(());
        }
        let pdev = self.pdev.as_ref().cloned().ok_or(ENODEV)?;
        let state = self.gfx_state.as_ref().cloned().ok_or(ENODEV)?;
        let signal = self.completion_signal.clone();
        if let Some(manager) = self.manager.as_mut() {
            // The doorbell that woke this worker is also the signal that a
            // submission may have retired; publish it to any waiter.
            if let Ok(Some(timestamps)) = manager.read_compute_completion() {
                signal.record(G17PComputeCompletion { timestamps });
            }
        }
        // Both RTKit roles notify the shared completion/event resources.
        // Consume GFX1 ingress too; otherwise a coalesced secondary-only edge
        // disappears when WorkItem1 is already running and no primary pending
        // count asks it to run again.
        let secondary_notifications = self.gfx1_state.as_ref().map_or(0, |secondary| {
            secondary.firmware_event_notifications.load(Ordering::Acquire)
        });
        let notifications_pending = secondary_notifications != 0 ||
            state.firmware_event_notifications.load(Ordering::Acquire) != 0;
        let recovery_pending = self
            .manager
            .as_ref()
            .ok_or(ENODEV)?
            .pending_primary_recovery()
            .is_some();
        let mut uma_release = None;
        let result = if recovery_pending {
            Ok(())
        } else if self.queue_setup_pending && !notifications_pending {
            let result = self.ensure_compute_queue();
            dev_info!(
                pdev.as_ref(),
                "G17P session: deferred SKSM QID 4 / selector 2 memory prepared after recovery-gate work; submissions={}\n",
                self.accepting_submissions
            );
            result
        } else {
            let manager = self.manager.as_mut().ok_or(ENODEV)?;
            match settle_primary_empty_recovery(&pdev, manager, &state, &signal) {
                Ok(release) => {
                    uma_release = release;
                    Ok(())
                }
                Err(error) => Err(error),
            }
        };
        let result = result.and_then(|()| {
            if let Some(release) = uma_release {
                self.service_render_usc_freelist_release(release)
            } else {
                Ok(())
            }
        });
        let result = result.and_then(|()| {
            if let Some(secondary) = self.gfx1_state.as_ref().cloned() {
                let batch = drain_primary_firmware_event_batch(&pdev,
                    self.manager.as_mut().ok_or(ENODEV)?, &secondary, &signal)?;
                if let Some(release) = batch.uma_release {
                    self.service_render_usc_freelist_release(release)?;
                }
                if let Some(generation) = batch.recovery_generation {
                    self.manager.as_mut().ok_or(ENODEV)?
                        .queue_primary_recovery(generation, batch.recovery_records)?;
                }
            }
            self.service_async_compute_completions()
        });
        if result.is_err() {
            state.crashed.store(true, Ordering::Release);
        }
        result
    }

    /// Service firmware events inline while a submission is in flight.
    ///
    /// The submit path holds the runtime mutex for the whole wait, so the
    /// workqueue event worker cannot acquire it until submit returns. The
    /// firmware's handshake (`state == 1`, "waiting for first response",
    /// raised together with the GPC power-up) therefore never gets an answer
    /// inside the submit window -- observed on J700 as the worker running
    /// 1.6s late, after the 625ms timeout had already expired. Drain and
    /// service here, where the lock is already held.
    fn service_firmware_events_inline(&mut self) -> Result<u32> {
        let pdev = self.pdev.as_ref().cloned().ok_or(ENODEV)?;
        let state = self.gfx_state.as_ref().cloned().ok_or(ENODEV)?;
        if state.firmware_event_notifications.load(Ordering::Acquire) == 0 {
            return Ok(0);
        }
        let signal = self.completion_signal.clone();
        let batch = {
            let manager = self.manager.as_mut().ok_or(ENODEV)?;
            drain_primary_firmware_event_batch(&pdev, manager, &state, &signal)?
        };
        if let Some(release) = batch.uma_release {
            self.service_render_usc_freelist_release(release)?;
        }
        if let Some(generation) = batch.recovery_generation {
            // Diagnostic cap: the J700 firmware re-raises a fault-free
            // recovery immediately after every final clear, ~500 times per
            // submit window. Capping lets us tell whether our own servicing
            // sustains the loop or the firmware would raise it regardless.
            let limit = *module_parameters::g17p_max_recoveries.value();
            // 0xffff = service none at all. Discriminates "the firmware halts
            // KSM when it *requests* recovery" from "our answer is what leaves
            // it halted".
            if limit == 0xffff {
                return Ok(batch.notification_count);
            }
            let serviced = INLINE_RECOVERIES_SERVICED.load(Ordering::Relaxed);
            if limit != 0 && serviced >= limit {
                if serviced == limit {
                    INLINE_RECOVERIES_SERVICED.store(limit + 1, Ordering::Relaxed);
                    dev_info!(
                        pdev.as_ref(),
                        "G17P recovery: inline servicing capped at {}; ignoring further requests\n",
                        limit
                    );
                }
                return Ok(batch.notification_count);
            }
            INLINE_RECOVERIES_SERVICED.fetch_add(1, Ordering::Relaxed);
            self.manager
                .as_mut()
                .ok_or(ENODEV)?
                .queue_primary_recovery(generation, batch.recovery_records)?;
            self.service_primary_recovery()?;
        }
        Ok(batch.notification_count)
    }

    pub(crate) fn service_primary_recovery(&mut self) -> Result {
        let pdev = self.pdev.as_ref().cloned().ok_or(ENODEV)?;
        let state = self.gfx_state.as_ref().cloned().ok_or(ENODEV)?;
        // Sample the queue/firmware agreement either side of the recovery.
        // A boot with no fault hands the retained graph to any number of
        // sequential clients; one GMMU fault in between and every later kick is
        // accepted, retired and never executed. Whatever moves across this
        // boundary is the mechanism, so measure it rather than infer it. Host
        // DRAM only -- see `log_compute_recovery_alignment`.
        self.log_recovery_alignment("pre-recovery");
        let registers = self.registers.as_ref();
        let manager = self.manager.as_mut().ok_or(ENODEV)?;
        let Some((generation, records)) = manager.pending_primary_recovery() else {
            return Ok(());
        };
        let result = complete_primary_real_recovery(
            &pdev,
            manager,
            registers,
            &state,
            generation,
            records,
        )
        .and_then(|()| manager.complete_pending_primary_recovery(generation, records));
        if result.is_err() {
            state.crashed.store(true, Ordering::Release);
            return result;
        }
        self.log_recovery_alignment("post-recovery");
        if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
            if let Err(error) = manager.resynchronise_compute_queue_after_recovery(queue) {
                dev_warn!(
                    pdev.as_ref(),
                    "G17P recovery: post-recovery queue resync failed ({:?}); the graph keeps its pre-recovery state\n",
                    error
                );
            }
        }
        result
    }

    /// One recovery-boundary sample of the retained queue, if there is one.
    fn log_recovery_alignment(&mut self, label: &'static str) {
        if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
            manager.log_compute_recovery_alignment(queue, label);
        }
    }

    pub(crate) fn drm_ref(&self) -> Result<ARef<AsahiDevice>> {
        self.drm.as_ref().cloned().ok_or(ENODEV)
    }

    pub(crate) fn is_crashed(&self) -> bool {
        self.gfx_state
            .as_ref()
            .is_some_and(|state| state.crashed.load(Ordering::Acquire))
            || self
                .gfx1_state
                .as_ref()
                .is_some_and(|state| state.crashed.load(Ordering::Acquire))
    }

    /// Rebuild a crashed session so a NEW client can still open the device.
    ///
    /// `file.rs::get_params` refuses with ENODEV while the session is crashed,
    /// and that check runs before any ioctl that could trigger the submit
    /// path's own recovery. So a single crashed session made every subsequent
    /// process fail at its first ioctl and the only way out was a reboot --
    /// which is exactly the cost a run of deliberately-failing workloads must
    /// not pay. `rebuild_session_in_place` installs fresh role state with
    /// `crashed` clear, so a recovery genuinely clears the condition.
    ///
    /// Bounded and loud: at most `CRASH_RECOVERY_ATTEMPTS` per module load, so
    /// a firmware that is really dead reports ENODEV instead of being restarted
    /// once per ioctl forever.
    pub(crate) fn recover_crashed_session_once(&mut self) -> bool {
        const CRASH_RECOVERY_ATTEMPTS: u32 = 4;
        if !self.is_crashed() {
            return false;
        }
        if *module_parameters::g17p_abandon_recovery.value() == 0
            || self.crash_recoveries >= CRASH_RECOVERY_ATTEMPTS
        {
            return true;
        }
        self.crash_recoveries += 1;
        let attempt = self.crash_recoveries;
        let result = g17_lifecycle::recover_persistent_runtime(self);
        if let Some(pdev) = self.pdev.as_ref() {
            match result {
                Ok(()) => dev_info!(
                    pdev.as_ref(),
                    "G17P session: crashed session rebuilt on attempt {}/{}; the device is open to new clients again\n",
                    attempt,
                    CRASH_RECOVERY_ATTEMPTS
                ),
                Err(error) => dev_err!(
                    pdev.as_ref(),
                    "G17P session: crashed-session rebuild attempt {}/{} failed ({:?}); reporting ENODEV\n",
                    attempt,
                    CRASH_RECOVERY_ATTEMPTS,
                    error
                ),
            }
        }
        self.is_crashed()
    }

    pub(crate) fn mark_firmware_cache_flush_ready(&self) -> Result {
        self.manager
            .as_ref()
            .ok_or(ENODEV)?
            .mark_firmware_cache_flush_ready();
        Ok(())
    }

    /// Create one client address space.
    ///
    /// Gated on the compute UAPI *stage* only -- which is what the selftest's
    /// "use g17p_test_stage=2" hint refers to -- and never on
    /// `accepting_submissions`. An `mmu::Vm` is page tables and nothing else:
    /// it needs no firmware queue, takes no UAT context slot until it is
    /// bound, and every client may hold one at the same time. Gating it on
    /// `accepting_submissions` made VM_CREATE fail with ENOTSUPP for the whole
    /// window between a session recovery and the workqueue that re-prepared
    /// QID 4 -- which is why a second client after a faulting one saw
    /// "VM_CREATE: Unknown error 524".
    pub(crate) fn new_user_vm(
        &mut self,
        id: u64,
        kernel_range: core::ops::Range<u64>,
    ) -> Result<mmu::Vm> {
        if !self.test_stage.enables_compute_uapi() {
            return Err(ENOTSUPP);
        }
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .new_user_vm(id, kernel_range)
    }

    pub(crate) fn map_timestamp_buffer(
        &self,
        bo: gem::ObjectRef,
        range: core::ops::Range<usize>,
    ) -> Result<mmu::KernelMapping> {
        if !self.test_stage.enables_compute_uapi() {
            return Err(ENOTSUPP);
        }
        self.manager
            .as_ref()
            .ok_or(ENODEV)?
            .map_timestamp_buffer(bo, range)
    }

    /// Build the retained QID 4 graph if this session has none.
    ///
    /// Normal startup leaves this graph absent so first render does not depend
    /// on compute-owned allocations or their live UAT mappings. The compute
    /// submit path calls this synchronously, including after recovery; only
    /// the explicit QID4 diagnostic asks the event worker to prepare it. This
    /// allocates and encodes mapped memory, but sends no firmware message and
    /// does not register the queue or issue compute work.
    pub(crate) fn ensure_compute_queue(&mut self) -> Result {
        if !self.test_stage.configures_qid4() {
            return Ok(());
        }
        if self.queue.is_none() {
            let drm = self.drm.as_ref().ok_or(ENODEV)?.clone();
            let queue = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .prepare_g17p_compute_queue(&drm)?;
            self.queue = Some(queue);
        }
        self.queue_setup_pending = false;
        self.accepting_submissions = self.test_stage.enables_compute_uapi();
        Ok(())
    }

    /// Number of DRM queue objects one firmware session can hand the hardware
    /// compute graph to before a full session recycle is forced.
    ///
    /// Each handoff retires the previous graph instead of freeing it (see
    /// `retired_queues`), so this is the bound on how much memory a session
    /// may park that way. The firmware's KSM restore set has 128 queue slots,
    /// which is the ceiling on *concurrently configured* QIDs -- a different
    /// quantity, and one this driver is nowhere near: it configures exactly
    /// one compute QID, because the SKSM tag/data port is unreachable from the
    /// AP (`g17p_sksm_mmio`) and the firmware only knows the QID the opening
    /// installed.
    const RETIRED_QUEUE_LIMIT: usize = 8;

    /// Take ownership of the hardware compute graph for `owner`.
    ///
    /// Same owner and nothing released in between: the retained graph and
    /// binding stay, which is what makes a steady-state resubmit possible.
    /// Anything else is a handoff.
    fn acquire_compute_owner(&mut self, owner: u64) -> Result {
        if self.compute_owner == Some(owner) && !self.compute_recycle_pending {
            return Ok(());
        }
        if self.compute_owner.is_some() || self.compute_recycle_pending {
            self.handoff_compute_queue()?;
        }
        self.compute_owner = Some(owner);
        Ok(())
    }

    /// Hand the one hardware compute queue from one DRM client to the next.
    ///
    /// ROOT CAUSE this exists to fix (second DRM client on one boot: submit
    /// accepted, no fault, no blamed queue, doorbell wait times out):
    ///
    ///  * The firmware installs a QID from the tag-15 ConfigUpdate's
    ///    queue-install flag at `+0x1a`. Firmware RE recorded in
    ///    `g17_compute::encode_compute_config_update` says the sole writer of
    ///    the per-queue slot `0x127628 + qid*0x28` -- together with
    ///    `slot[+0x00] = 1`, the per-data-master active bitmap and the
    ///    `sgx+0x21008` queue-base store -- is reachable only when that flag is
    ///    non-zero, and that leaving it set on an ALREADY-installed queue
    ///    forces a disable/reprogram/enable cycle on the KSMFE queue, whose
    ///    observed symptom is precisely "published fine and then never
    ///    completed".
    ///  * Nothing on the host can un-install it. `unregister_compute_sksm_queue`
    ///    issues `disable_pair` through `g17p_sksm_publish_write_pair`, which is
    ///    a logged no-op whenever `g17p_sksm_mmio == 0` -- the default, and the
    ///    only setting that does not SError on J700. So after the first client's
    ///    first bind, QID 4 stays installed in the firmware and in KSMFE for the
    ///    rest of the boot.
    ///  * Both previous handoff modes then re-ran the FIRST-BIND chronology on
    ///    that still-installed QID with the install flag set again -- mode 1
    ///    because the replacement graph is a fresh `PreparedMemory` object, and
    ///    mode 0 because a rebuilt session's queue is fresh for the same reason.
    ///    That is why the two modes fail identically, and why the failure is
    ///    deterministic on the SECOND binding rather than being a race.
    ///
    /// The fix is to stop rebuilding the queue at all. The QID 4 graph, its
    /// item ring, its SKSM stamp sequence and the firmware's installed queue
    /// slot are properties of the firmware SESSION, not of a DRM client; the
    /// only per-client state is the user-VM binding and the timestamp page.
    /// Releasing just those and letting the next client bind its own VM makes
    /// its first submission take the same steady-state chronology (tag 3 ->
    /// 15 with the install flag CLEAR -> 14 -> 16 -> CL_2, appended at the
    /// live producer) that already runs 300/300 within one client.
    ///
    /// Falls back to the old rebuild path whenever the retained graph cannot
    /// be trusted: a crashed session, a submission still in flight, a queue the
    /// firmware never installed, or an exhausted quarantine budget. NOTE that
    /// the fallback is itself broken for the NEXT client, for the reason above
    /// -- it is a last resort, not a safe default, and the
    /// `G17P TAG15-INSTALL ... flag=1` line names it whenever it happens.
    fn handoff_compute_queue(&mut self) -> Result {
        // A closed core-power / KSM pause gate is the one condition retention
        // cannot survive. Measured: before a fault the GPU executes (retire
        // duration 0xda-0xdd), after a fault-blamed recovery every submission
        // on the retained graph retires with duration 0 having executed
        // nothing, the pre/post recovery alignment is byte-identical, and the
        // one host lever we have (device-control IdlePowerOff(0), opcode 0x0a
        // arg 0 -- `g17p_power_wire=1`) does not clear it. Nothing about the
        // QUEUE diverged, so resynchronising the queue cannot help; the only
        // state in which execution has ever been observed to return is a fresh
        // firmware session. So rebuild -- and rebuild with a rotated QID, or
        // the rebuilt session's first bind re-asserts the tag-15 install flag
        // on a QID the firmware still has installed and fails for the
        // completely different reason this work started from.
        if self.compute_gate_suspect && *module_parameters::g17p_zero_duration.value() >= 2 {
            // Time the rebuild. Whether thousands of these become a bottleneck
            // across a conformance run is a question about milliseconds, and
            // nobody has measured one; guessing at a cheaper recovery before
            // knowing what the expensive one costs is how effort gets wasted.
            let started = Instant::<Monotonic>::now();
            let result = self.recycle_compute_queue();
            let elapsed_us = started.elapsed().as_micros_ceil();
            self.compute_gate_suspect = false;
            if let Some(pdev) = self.pdev.as_ref() {
                let (installs, zeros, _, retains) = g17_manager::g17p_mark_counts();
                let rebuilds = g17_manager::G17P_MARK_GATE_REBUILDS
                    .fetch_add(1, Ordering::Relaxed)
                    + 1;
                dev_err!(
                    pdev.as_ref(),
                    "G17PMARK gate-rebuild i={} z={} r={} h={} took {}us ok={} -- firmware session rebuilt after a zero-duration retire instead of retaining the gated graph\n",
                    installs,
                    zeros,
                    rebuilds,
                    retains,
                    elapsed_us,
                    result.is_ok(),
                );
            }
            return result;
        }
        if *module_parameters::g17p_queue_recycle.value() >= 2
            && self.compute_queue_can_rebind()
        {
            return self.rebind_compute_queue();
        }
        self.recycle_compute_queue()
    }

    /// How many abandoned clients' bindings one firmware session may park.
    ///
    /// Each entry pins one failed client's page tables and mapped GEM objects
    /// until both CPUs stop, so this bounds how much memory a run of failing
    /// workloads may hold. Past the limit the handoff falls back to the full
    /// rebuild, which stops both CPUs and frees the whole quarantine at once.
    const QUARANTINED_BINDING_LIMIT: usize = 64;

    /// Whether the retained QID 4 graph may be handed on with a bare rebind.
    ///
    /// `compute_abandoned` is deliberately NOT a refusal any more. It used to
    /// be, on the reasoning that only a CPU stop makes releasing an
    /// outstanding client's binding safe -- but the fallback that reasoning
    /// selects is a queue REBUILD, and a rebuilt queue re-asserts the tag-15
    /// install flag against a QID the firmware already has installed, which is
    /// the very failure `handoff_compute_queue` exists to avoid. So one
    /// faulting client poisoned every client after it for the rest of the boot.
    ///
    /// The safety property is kept a different way: the outgoing binding is
    /// QUARANTINED rather than dropped (see `rebind_compute_queue`), so nothing
    /// the firmware might still name is unmapped or freed, and no CPU stop is
    /// needed to establish that.
    ///
    /// `pending_compute` still refuses: that is a submission this driver is
    /// actively waiting on, not one it has given up on.
    fn compute_queue_can_rebind(&self) -> bool {
        if self.is_crashed() || self.pending_compute.is_some() {
            return false;
        }
        if self.compute_abandoned
            && (*module_parameters::g17p_abandon_recovery.value() == 0
                || self.quarantined_bindings.len() >= Self::QUARANTINED_BINDING_LIMIT)
        {
            return false;
        }
        match (self.queue.as_ref(), self.manager.as_ref()) {
            (Some(queue), Some(manager)) => manager.compute_queue_is_installed(queue),
            _ => false,
        }
    }

    /// Release only the outgoing client's state and keep the installed graph.
    ///
    /// Order matters: the owned timestamp page is a mapping in the outgoing
    /// VM, so it goes before the binding that holds that VM alive. Dropping the
    /// binding releases UAT slot 1 and lets `VmInner::drop` retract the ctx2/3
    /// aliases it published; the next `bind_compute_user_vm` republishes them
    /// for the incoming VM before anything is kicked.
    ///
    /// Nothing firmware-visible is touched: no SKSM transaction, no tag-15
    /// install, no CPU stop. The firmware keeps naming the same entry ring,
    /// descriptor storage and channel control pages, none of which are freed.
    fn rebind_compute_queue(&mut self) -> Result {
        self.compute_owner = None;
        self.compute_recycle_pending = false;
        let abandoned = self.compute_abandoned;
        drop(self.compute_timestamps.take());
        if abandoned {
            // The outgoing client published a kick that was never seen to
            // retire. Park its memory instead of dropping it -- see
            // `G17PQuarantinedComputeBinding` -- and repair the accounting the
            // abandoned kick leaked on the retained graph, so the incoming
            // client submits against a queue that is merely FURTHER ALONG
            // rather than one that is broken.
            if let Some(binding) = self.compute_binding.take() {
                self.quarantined_bindings.push(binding.quarantine(), GFP_KERNEL)?;
            }
            if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
                manager.resynchronise_compute_queue_after_abandon(queue)?;
            }
            self.compute_abandoned = false;
            if let Some(pdev) = self.pdev.as_ref() {
                dev_info!(
                    pdev.as_ref(),
                    "G17P compute: handoff from an ABANDONED submission; {} client binding(s) quarantined this session\n",
                    self.quarantined_bindings.len()
                );
            }
        } else {
            drop(self.compute_binding.take());
        }
        let snapshot = self.queue.as_mut().map(|queue| queue.handoff_snapshot());
        if let (Some(pdev), Some((ordinal, stamp, ring))) = (self.pdev.as_ref(), snapshot) {
            let (installs, zeros, rebuilds, _) = g17_manager::g17p_mark_counts();
            let retains = g17_manager::G17P_MARK_RETAINED_HANDOFFS
                .fetch_add(1, Ordering::Relaxed)
                + 1;
            dev_info!(
                pdev.as_ref(),
                "G17PMARK handoff-retained i={} z={} r={} h={} ordinal={} stamp={:#x} ring-producer={} -- graph kept, next submit takes the steady-state chronology with the tag-15 install flag clear\n",
                installs,
                zeros,
                rebuilds,
                retains,
                ordinal,
                stamp,
                ring
            );
        }
        Ok(())
    }

    /// Release the hardware compute graph held by `owner`, if it holds it.
    ///
    /// Called from the DRM queue object's `Drop`, i.e. on `queue_destroy` and
    /// on file close -- including the close the kernel performs for a client
    /// that died mid-submission.
    ///
    /// This ONLY marks the graph as released. It deliberately does not drop the
    /// binding: `install_t8140_compute_context_alias` left this VM's root in
    /// hardware contexts 2 and 3, and nothing rewrites those two entries until
    /// the *next* client binds. Freeing the page tables here would leave the
    /// firmware's compute context pointing at memory the allocator has taken
    /// back. The binding is therefore released inside `handoff_compute_queue`
    /// instead -- immediately before the next bind reinstalls those roots, or,
    /// on the full path, after both CPUs have stopped.
    pub(crate) fn release_compute_owner(&mut self, owner: u64) {
        if self.compute_owner != Some(owner) {
            return;
        }
        self.compute_owner = None;
        self.compute_recycle_pending = true;
        if let Some(pdev) = self.pdev.as_ref() {
            dev_info!(
                pdev.as_ref(),
                "G17P compute: client queue {} released the QID4 graph (outstanding={}); recycle deferred to the next submit\n",
                owner,
                self.compute_abandoned || self.pending_compute.is_some()
            );
        }
    }

    /// Make the hardware compute graph fit for a first bind by a new client.
    ///
    /// NOTE: both modes below re-run the FIRST-BIND chronology on the next
    /// submit, which republishes the tag-15 queue-install flag against a QID
    /// the firmware still has installed -- see `handoff_compute_queue` for why
    /// that is the second-client failure. They are kept as the fallback for a
    /// graph that genuinely cannot be handed on (crashed session, abandoned or
    /// in-flight submission, queue never installed) and as an A/B lever.
    ///
    /// Two modes, selected by `g17p_queue_recycle`:
    ///  * 1 (default): recycle the queue graph alone. The old graph is taken
    ///    off the SKSM bridge, retired (not freed), and replaced with a fresh
    ///    `PreparedMemory` graph. Neither ASC is restarted, so a client handoff
    ///    costs an allocation rather than a firmware session.
    ///  * 0: the previously shipped behaviour -- a full
    ///    `recover_persistent_runtime`, stopping both CPUs and rebuilding the
    ///    session. Slower by orders of magnitude, but it is the path a VM
    ///    change has always taken.
    /// A crashed session, a queue-only recycle that errors, and an exhausted
    /// retirement budget all fall through to mode 0.
    fn recycle_compute_queue(&mut self) -> Result {
        self.compute_owner = None;
        self.compute_recycle_pending = false;
        // `compute_gate_suspect` joins this set deliberately: the queue-only
        // recycle keeps both ASCs running, and a gated GPU is exactly what a
        // running session cannot be talked out of.
        let outstanding =
            self.compute_abandoned || self.pending_compute.is_some() || self.compute_gate_suspect;
        let queue_only = *module_parameters::g17p_queue_recycle.value() != 0
            && !outstanding
            && !self.is_crashed()
            && self.retired_queues.len() < Self::RETIRED_QUEUE_LIMIT;
        if queue_only {
            drop(self.pending_compute.take());
            drop(self.compute_timestamps.take());
            drop(self.compute_binding.take());
            match self.recycle_compute_queue_in_place() {
                Ok(()) => return Ok(()),
                Err(error) => {
                    if let Some(pdev) = self.pdev.as_ref() {
                        dev_err!(
                            pdev.as_ref(),
                            "G17P compute: queue-only recycle failed ({:?}); rebuilding the session\n",
                            error
                        );
                    }
                }
            }
        }
        // The full path stops both CPUs before `release_runtime_objects`
        // frees anything, which is what makes releasing an outstanding
        // client's binding safe.
        if let Err(error) = g17_lifecycle::recover_persistent_runtime(self) {
            // The rebuild has already dropped both CPUs and both RTKit owners
            // and never reached `resume_submissions`, so the runtime is now
            // gutted: nothing here can put it back. Say so loudly and mark the
            // session crashed, which is the one state
            // `recover_crashed_session_once` will retry from when a later
            // client opens the device -- otherwise the failure is silent and
            // every subsequent ioctl returns a bare ENODEV.
            if let Some(pdev) = self.pdev.as_ref() {
                dev_err!(
                    pdev.as_ref(),
                    "G17PMARK rebuild-failed err={:?}; the firmware session is gone (no CPUs, no RTKit) and this device is dead until a rebuild succeeds\n",
                    error
                );
            }
            if let Some(state) = self.gfx_state.as_ref() {
                state.crashed.store(true, Ordering::Release);
            }
            return Err(error);
        }
        self.compute_abandoned = false;
        self.ensure_compute_queue()
    }

    fn recycle_compute_queue_in_place(&mut self) -> Result {
        let mut retired_queue_id = None;
        if let Some(mut queue) = self.queue.take() {
            retired_queue_id = Some(queue.queue_id() as usize);
            let unregister = match (self.manager.as_ref(), self.registers.as_ref()) {
                (Some(manager), Some(registers)) => {
                    manager.unregister_compute_sksm_queue(registers, &mut queue)
                }
                _ => Err(ENODEV),
            };
            // Retire the graph either way. It must not be freed while the
            // firmware may still name it, and a failed disable is exactly the
            // case where that is most likely.
            self.retired_queues.push(queue, GFP_KERNEL)?;
            unregister?;
        }
        // Restore the firmware's per-QID progress record before the
        // replacement graph is built.
        //
        // `prepare_g17p_compute_queue` hands back a graph whose producer is
        // re-seeded to `geometry.timestamp_seed()`, i.e. the first kick a new
        // client publishes carries the same low stamp the previous owner's
        // first kick did. The firmware still has the RETIRING owner's
        // last-submitted stamp recorded for this QID in the 128-entry table
        // published at primary main-config `+0x274` (bundle view 4). A stamp
        // at or below that is the "already retired" condition -- the kick is
        // accepted and never run, so the
        // submit reaches the doorbell wait and times out with nothing faulted.
        //
        // Nothing else on this path clears it:
        //  * `reset_empty_sksm_last_submitted_hw_timestamps` is reached only
        //    from the recovery service, and only for `generation == 0`;
        //  * the B2 republication `publish_compute_b2_once` performs (the fresh
        //    graph comes back `Pending`) writes region_1 at 0x10..0x20, which
        //    is QID 1's slot -- QID 4's sits at 0x40 and is untouched;
        //  * the SKSM disable/configure transactions either side of this are
        //    suppressed no-ops whenever `g17p_sksm_mmio == 0`, which is the
        //    default and the only setting that does not SError on J700, so the
        //    bridge never re-arms anything either.
        //
        // The full `recover_persistent_runtime` path does not need this: it
        // stops both CPUs and takes the generation-0 recovery, which publishes
        // the same 128 zero entries. That asymmetry is exactly why a second DRM
        // client works with `g17p_queue_recycle=0` and not with the default.
        if let Some(qid) = retired_queue_id {
            let pdev = self.pdev.as_ref().cloned();
            if let Some(manager) = self.manager.as_mut() {
                let before = manager.g17p_last_submitted_hw_timestamp(qid);
                manager.reset_empty_sksm_last_submitted_hw_timestamps()?;
                let after = manager.g17p_last_submitted_hw_timestamp(qid);
                if let Some(pdev) = pdev.as_ref() {
                    match (before, after) {
                        (Ok((valid_before, stamp_before)), Ok((valid_after, stamp_after))) => {
                            dev_info!(
                                pdev.as_ref(),
                                "G17P compute: handoff reset QID {} last-submitted HW timestamp valid {} -> {} stamp {:#x} -> {:#x}\n",
                                qid,
                                valid_before,
                                valid_after,
                                stamp_before,
                                stamp_after,
                            );
                        }
                        _ => dev_info!(
                            pdev.as_ref(),
                            "G17P compute: handoff reset the last-submitted HW timestamp table; QID {} read-back unavailable\n",
                            qid,
                        ),
                    }
                }
            }
        }
        // Kept as-is deliberately, so this change stays single-variable.
        // `completion_indices` is session state, not queue state, and the
        // session did not restart here -- but it is only ever written and never
        // read, so the reset is inert. Flagged rather than removed: if a reader
        // is ever added, an in-place handoff must NOT reset it, because the
        // firmware's completion ring keeps counting across one.
        // `release_runtime_objects` resets it correctly, after both CPUs stop.
        self.completion_indices = g17_completion::QueueIndices {
            done: 0,
            read: 0,
            write: 0,
        };
        self.queue_setup_pending = true;
        self.ensure_compute_queue()?;
        if let Some(pdev) = self.pdev.as_ref() {
            dev_info!(
                pdev.as_ref(),
                "G17P compute: QID4 graph recycled in place; {} retired graph(s) held by this session\n",
                self.retired_queues.len()
            );
        }
        Ok(())
    }

    /// Build, publish, notify, and synchronously retire one translated user
    /// render through the Linux-owned G17P queue graph.
    /// Wait for the render to complete, servicing firmware events between
    /// polls when `g17p_render_service_events` is set.
    ///
    /// WHY THIS EXISTS. The render submit runs under the runtime mutex --
    /// `g17_drm.rs` calls `submit_translated_render` on a locked guard and the
    /// wait happens inside that call -- so the workqueue item that would drain
    /// the firmware event ring blocks on `mutex().lock()` for the whole wait.
    /// Measured consequence: the firmware halts, posts a type-4
    /// `kAGFIFirmwareEventTypeGPURestart` naming stamp slot 0, raises the RTKit
    /// notification (four seen, role=GFX1) -- and the ring still reads
    /// `producer=1 consumer=0 pending=1` when the wait gives up. The firmware
    /// spins until the host writes 2 into `status_b[0x4900]`; never answering
    /// leaves KSM halted forever.
    ///
    /// The fix mirrors `submit_translated_compute`, which polls with the mutex
    /// held and therefore calls `service_firmware_events_inline` from inside
    /// its own loop rather than relying on the worker. That is deliberately NOT
    /// the same as releasing the lock: no locking changes here, so compute and
    /// the DRM-level wait are untouched, and `service_firmware_events_inline`
    /// already does the `queue_primary_recovery(generation, ..)` bookkeeping
    /// that a bare drain would skip.
    ///
    /// SCOPE: this makes a render halt RECOVERABLE. It does not make a render
    /// succeed. The completion records were read directly from the KSM rings
    /// and were genuinely empty, so nothing about this makes the work run.
    fn wait_render_completion_servicing_events(
        &mut self,
    ) -> Result<g17_completion::PairedRenderCompletion> {
        if *module_parameters::g17p_render_service_events.value() == 0 {
            let completion = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .wait_translated_render_completion()?;
            if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
                finish_g17p_render_trace(pdev.as_ref(), manager,
                    crate::g17_trace_capture::Phase::AfterPairRetirement);
            }
            let release = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .retire_render_usc_freelist_at_completion()?;
            if let Some(release) = release {
                self.service_render_usc_freelist_release(release)?;
            }
            return Ok(completion);
        }
        let mut serviced = 0u32;
        let mut service_failures = 0u32;
        for _ in 0..g17_manager::G17PManagerConstruction::RENDER_COMPLETION_POLLS {
            match self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .poll_translated_render_completion()?
            {
                g17_manager::G17PRenderPollOutcome::Complete(completion) => {
                    // Drain pending firmware notifications BEFORE retiring the
                    // USC FList references. The lifecycle holds three: the
                    // async-grow request, the TA and the 3D. The firmware's
                    // type-13 async-grow completion retires the first (3 -> 2)
                    // and only then may this paired completion retire the other
                    // two. Returning straight out of the poll skipped that, so
                    // the retire saw references=3 and refused with WrongPhase,
                    // which the caller reported as a bare EIO -- AFTER the GPU
                    // had finished the whole render. Ordering only: nothing is
                    // retired here that was not already complete.
                    match self.service_firmware_events_inline() {
                        Ok(count) => serviced += count,
                        Err(error) => {
                            if let Some(pdev) = self.pdev.as_ref() {
                                dev_warn!(
                                    pdev.as_ref(),
                                    "G17P render: event service at completion failed ({:?})\n",
                                    error
                                );
                            }
                        }
                    }
                    if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
                        finish_g17p_render_trace(pdev.as_ref(), manager,
                            crate::g17_trace_capture::Phase::AfterPairRetirement);
                    }
                    let release = self
                        .manager
                        .as_mut()
                        .ok_or(ENODEV)?
                        .retire_render_usc_freelist_at_completion()?;
                    if let Some(release) = release {
                        self.service_render_usc_freelist_release(release)?;
                    }
                    if serviced != 0 {
                        if let Some(pdev) = self.pdev.as_ref() {
                            dev_info!(
                                pdev.as_ref(),
                                "G17P render: completed after servicing {} firmware event notifications\n",
                                serviced
                            );
                        }
                    }
                    return Ok(completion);
                }
                g17_manager::G17PRenderPollOutcome::Retry => continue,
                g17_manager::G17PRenderPollOutcome::Pending => {}
            }
            match self.service_firmware_events_inline() {
                Ok(count) => serviced += count,
                Err(error) => {
                    if service_failures == 0 {
                        if let Some(pdev) = self.pdev.as_ref() {
                            dev_warn!(
                                pdev.as_ref(),
                                "G17P render: inline event service failed ({:?})\n",
                                error
                            );
                        }
                    }
                    service_failures += 1;
                }
            }
            fsleep(Delta::from_millis(1));
        }
        if let Some(pdev) = self.pdev.as_ref() {
            dev_err!(
                pdev.as_ref(),
                "G17P render: wait timed out; serviced {} notifications, {} service failures\n",
                serviced,
                service_failures
            );
        }
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .report_render_wait_timeout();
        Err(ETIMEDOUT)
    }

    /// Poll one independently owned render slot while holding the runtime
    /// mutex only for this bounded observation/service transaction.  The DRM
    /// caller sleeps with the mutex released, allowing the other slot to
    /// publish and allowing asynchronous compute retirement to proceed.
    pub(crate) fn poll_deferred_render_completion(
        &mut self,
        render_slot: u8,
    ) -> Result<Option<g17_completion::PairedRenderCompletion>> {
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .select_render_slot(render_slot)?;
        let outcome = self
            .manager
            .as_mut()
            .ok_or(ENODEV)?
            .poll_translated_render_completion()?;
        match outcome {
            g17_manager::G17PRenderPollOutcome::Complete(completion) => {
                let _ = self.service_firmware_events_inline();
                if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
                    finish_g17p_render_trace(
                        pdev.as_ref(),
                        manager,
                        crate::g17_trace_capture::Phase::AfterPairRetirement,
                    );
                }
                let release = self
                    .manager
                    .as_mut()
                    .ok_or(ENODEV)?
                    .retire_render_usc_freelist_at_completion()?;
                if let Some(release) = release {
                    self.service_render_usc_freelist_release(release)?;
                }
                if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
                    log_g17p_status_a(pdev.as_ref(), manager, "render-post-wait");
                    probe_g17p_ktrace(pdev.as_ref(), manager, "render-post-wait");
                    dump_g17p_firmware_log(pdev.as_ref(), manager, "render-post-wait");
                    manager.log_render_status_pages("post-completion");
                }
                Ok(Some(completion))
            }
            g17_manager::G17PRenderPollOutcome::Retry => Ok(None),
            g17_manager::G17PRenderPollOutcome::Pending => {
                let _ = self.service_firmware_events_inline();
                Ok(None)
            }
        }
    }

    pub(crate) fn abandon_deferred_render(
        &mut self,
        render_slot: u8,
    ) -> Result {
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .select_render_slot(render_slot)?;
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .report_render_wait_timeout();
        self.abandon_render_submission_after_timeout();
        Ok(())
    }

    /// Wait only for the two TA-owned timestamps used by the split-launch
    /// diagnostic. The sample path is retained DRAM; it performs no SGX MMIO,
    /// completion acknowledgement, recovery service, or fragment publication.
    /// The bound is deliberately below the measured ~84ms fragment recovery,
    /// so a failed TA phase cannot leave firmware halted at recovery state 1
    /// while this diagnostic keeps polling.
    /// Release a timed-out render without tearing down the runtime.
    ///
    /// Drops this submission's retained render state so the next submission
    /// builds its own, and leaves the session accepting work.
    fn abandon_render_submission_after_timeout(&mut self) {
        if let Some(pdev) = self.pdev.as_ref() {
            dev_warn!(
                pdev.as_ref(),
                "G17P render: abandoning the timed-out submission; runtime stays up\n"
            );
        }
        // Deliberately release nothing here. The firmware may still hold
        // references to this submission's graph, and freeing it under a live
        // pair of firmware CPUs is how you turn a stalled render into a DMA
        // fault. The next submission reuses or rebuilds through the retained
        // paths it already has; the only thing this path must guarantee is
        // that the session keeps accepting work.
    }

    fn wait_split_render_tiling_completion(&mut self) -> Result<[u64; 2]> {
        const POLLS: usize = 50;
        for _ in 0..POLLS {
            if let Some(timestamps) = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .split_render_tiling_completion()?
            {
                return Ok(timestamps);
            }
            fsleep(Delta::from_millis(1));
        }
        if let Some(pdev) = self.pdev.as_ref() {
            dev_err!(
                pdev.as_ref(),
                "G17P render split-launch: TA did not complete within {} DRAM polls; 3D remains unpublished\n",
                POLLS
            );
        }
        Err(ETIMEDOUT)
    }

    pub(crate) fn submit_translated_render(
        &mut self,
        vm: &mmu::Vm,
        render_slot: u8,
        defer_completion: bool,
        execution_context: Arc<mmu::T8140ComputeExecutionContext>,
        command: &g17_uapi::TranslatedRenderCommand,
        timestamps: [u64; 4],
        timestamp_aliases: KVec<mmu::KernelMapping>,
    ) -> Result {
        if !self.accepting_submissions {
            return Err(ENOTSUPP);
        }
        let queue_pair = g17_resources::G17PRenderQueuePair::for_slot(render_slot)
            .ok_or(EINVAL)?;
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .select_render_slot(render_slot)?;
        let native_doorbell_mode =
            *module_parameters::g17p_render_native_doorbells.value();
        let native_doorbells = native_doorbell_mode != 0;
        let split_launch = *module_parameters::g17p_render_split_launch.value() != 0;
        if native_doorbell_mode > 3 || (native_doorbells && split_launch) {
            if let Some(pdev) = self.pdev.as_ref() {
                dev_err!(
                    pdev.as_ref(),
                    "G17P render: native doorbell pair and split launch are mutually exclusive\n"
                );
            }
            return Err(EINVAL);
        }
        if self.is_crashed() {
            g17_lifecycle::recover_persistent_runtime(self)?;
        }
        // Settle an older asynchronous release before any recovery/control
        // publication can advance the same ring past its exact target.
        self.settle_render_usc_freelist_release_before_submission()?;
        // Never publish while a firmware recovery is open. The compute path
        // has done this since the recovery work; render never did, and the
        // firmware behaviour it guards against is exactly render's symptom:
        // while `status_b[0x4900] != 0` the scheduler dispatches only the
        // unhalt event and STASHES EVERY WORK-SCAN BIT, so nothing published
        // can run -- and the unhalt's 128-queue restore rewrites
        // 0x21010/0x21018/0x21020 for every queue from the halt-time
        // snapshot, erasing any tag-14 AddKicks published while it was open.
        // A render that is admitted, armed on its pipe and never launched is
        // what that produces.
        self.settle_open_recovery_before_publication()?;
        // The legacy synchronous compute path keeps its client resources in
        // fixed contexts 2/3; this render owns a distinct application-GART
        // context and embeds its complete low-view closure there.  They no
        // longer alias, so neither an idle nor an in-flight QID4 binding is a
        // reason to hand the compute graph back before render publication.
        self.manager.as_mut().ok_or(ENODEV)?.handoff_retained_render_vm(vm)?;
        let recycle = self
            .manager
            .as_ref()
            .ok_or(ENODEV)?
            .retained_render_needs_recycle(vm);
        if recycle {
            g17_lifecycle::recover_persistent_runtime(self)?;
        }
        let drm = self.drm.as_ref().ok_or(ENODEV)?.clone();
        // Disjoint field borrows: `registers` and `manager` are separate fields,
        // so a shared borrow of one alongside a mutable borrow of the other is
        // accepted. The SKSM half of a render submit needs both.
        let registers = self.registers.as_ref().ok_or(ENODEV)?;
        let prepared = self
            .manager
            .as_mut()
            .ok_or(ENODEV)?
            .begin_translated_render(
                &drm,
                vm,
                queue_pair,
                execution_context,
                command,
                timestamps,
                timestamp_aliases,
                registers,
            )?;
        // Copy only before the FList wake and while both outers are deferred.
        // Never insert this scan between producer publication and doorbell.
        if let Some(manager) = self.manager.as_mut() {
            if let Err(error) = manager.capture_prepared_render_trace(
                native_doorbell_mode == 0 && !split_launch,
            ) {
                if let Some(pdev) = self.pdev.as_ref() {
                    dev_warn!(pdev.as_ref(), "G17P prepared binary capture failed: {:?}\n", error);
                }
            }
        }
        if let Err(error) = self.publish_render_usc_freelist_request() {
            return match g17_lifecycle::recover_persistent_runtime(self) {
                Ok(()) => Err(error),
                Err(recovery_error) => Err(recovery_error),
            };
        }
        if *module_parameters::g17p_power_wire.value() == 1 {
            if let Err(error) = self.wire_g17p_submit_gpu_power() {
                return match g17_lifecycle::recover_persistent_runtime(self) {
                    Ok(()) => Err(error),
                    Err(recovery_error) => Err(recovery_error),
                };
            }
        }
        // Status-A is a plain DRAM read of the host-owned primary state grid --
        // no sgx MMIO, so it is safe with the GPU cores still gated. `scan-active`
        // going 0 -> 1 across the doorbell is one of the three signals that
        // flipped when compute started executing.
        if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
            log_g17p_status_a(pdev.as_ref(), manager, "render-pre-doorbell");
            // The render path never drained the KTrace ring, so an absent code
            // said nothing about the firmware -- only that nobody had looked.
            // Consume whatever is already there, so every record the post-wait
            // drain reports is attributable to THIS submission. Both helpers
            // are DRAM reads of host-allocated objects; unlike `gpc-state` they
            // carry no sgx/SError hazard on a path where the cores may never
            // have come up.
            probe_g17p_ktrace(pdev.as_ref(), manager, "render-pre-doorbell");
            probe_g17p_reports(pdev.as_ref(), manager, "render-pre-doorbell");
        }
        if let Some(manager) = self.manager.as_mut() {
            manager.log_render_status_pages("pre-doorbell");
            manager.log_render_sksm_entries("pre-doorbell");
            if native_doorbell_mode == 2 || native_doorbell_mode == 3 {
                manager.log_render_outer_channels("pre-fragment-doorbell");
            }
        }
        if *module_parameters::g17p_render_ksm_probe.value() != 0 {
            // Reacquire after the mutable power/control calls above so this
            // diagnostic borrow does not artificially span them.
            let registers = self.registers.as_ref().ok_or(ENODEV)?;
            match registers.g17p_render_fault_irq_snapshot() {
                Ok(Some(snapshot)) => dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P fault IRQ @render-pre-fragment-doorbell: gpc-state={:#x} info={:#018x} addr-word={:#018x} gate={:#018x} sub-status={:#018x} irq-status={:#018x} bit35={}\n",
                    snapshot.gpc_state,
                    snapshot.fault_info,
                    snapshot.fault_addr_word,
                    snapshot.requestor_gate,
                    snapshot.sub_status,
                    snapshot.irq_status,
                    (snapshot.irq_status >> 35) & 1,
                ),
                Ok(None) => dev_warn!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P fault IRQ @render-pre-fragment-doorbell: skipped because gpc-state=0\n"
                ),
                Err(error) => dev_warn!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P fault IRQ @render-pre-fragment-doorbell: snapshot failed ({:?})\n",
                    error,
                ),
            }
            if let Err(error) = registers
                .g17p_log_fault_requestors_pre_ack("render-pre-fragment-doorbell")
            {
                dev_warn!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P fault requestor @render-pre-fragment-doorbell: sweep failed ({:?})\n",
                    error,
                );
            }
        }
        if *module_parameters::g17p_seed_work_channel.value() != 0 {
            if let Some(registers) = self.registers.as_ref() {
                let value = u64::from(*module_parameters::g17p_seed_work_channel.value());
                match registers.g17p_seed_work_channel(value) {
                    Ok(()) => dev_info!(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        "G17P render: DIAGNOSTIC seeded KSM work channel 0x21168 = {:#x}\n",
                        value,
                    ),
                    Err(error) => dev_warn!(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        "G17P render: DIAGNOSTIC work-channel seed failed ({:?})\n",
                        error,
                    ),
                }
            }
        }
        if *module_parameters::g17p_fw_trace_classes.value() != 0 {
            // 0x17780c bits 1,2 and 0x17780d bit 1, inside the qword at
            // 0x177808: byte 4 bits 1,2 -> qword bits 33,34; byte 5 bit 1 ->
            // qword bit 41.
            const CLASS_QWORD_LINK: u64 = 0xffff_fc00_0017_7808;
            // Mode 1 arms the classes the firmware RE named (0x17780c bits
            // 1-2, 0x17780d bit 1). Mode 2 arms EVERY class in both bytes:
            // mode 1 produced ch=1 but no ch=9, so the one-bit-per-class
            // reading is unconfirmed and the cheapest way to find 0x909 is to
            // stop guessing the mapping.
            let class_bits: u64 = if *module_parameters::g17p_fw_trace_classes.value() >= 2 {
                0x0000_ffff_0000_0000
            } else {
                (1u64 << 33) | (1u64 << 34) | (1u64 << 41)
            };
            const CLASS_BITS_UNUSED: u64 = 0;
            let phys = g17_manager::g17p_gfx_link_to_physical(CLASS_QWORD_LINK);
            match crate::pgtable::LiveFirmwareU64Probe::new(phys)
                .and_then(|p| p.set_bits(class_bits))
            {
                Ok((before, after)) => dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P render: DIAGNOSTIC armed fw trace classes {:#018x} -> {:#018x}\n",
                    before,
                    after,
                ),
                Err(error) => dev_warn!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P render: DIAGNOSTIC fw trace class arm failed ({:?})\n",
                    error,
                ),
            }
        }
        if *module_parameters::g17p_ksm_cold_arm.value() != 0 {
            if let Some(registers) = self.registers.as_ref() {
                match registers.g17p_ksm_cold_arm() {
                    Ok(()) => dev_info!(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        "G17P render: DIAGNOSTIC host KSM cold arm written (0x21140 = 1<<63)\n"
                    ),
                    Err(error) => dev_warn!(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        "G17P render: DIAGNOSTIC host KSM cold arm failed ({:?})\n",
                        error,
                    ),
                }
            }
        }
        if *module_parameters::g17p_admit_arm.value() != 0 {
            if let Some(registers) = self.registers.as_ref() {
                let both = *module_parameters::g17p_admit_arm.value() >= 2;
                let mut queues = KVec::new();
                queues.push(queue_pair.tiling as u8, GFP_KERNEL)?;
                if both {
                    queues.push(queue_pair.fragment as u8, GFP_KERNEL)?;
                }
                for qid in queues {
                    match registers.g17p_arm_queue_admit(qid) {
                        Ok((before, after)) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render: DIAGNOSTIC admit-arm qid {} descriptor {:#018x} -> {:#018x}\n",
                            qid,
                            before,
                            after,
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render: DIAGNOSTIC admit-arm qid {} failed ({:?})\n",
                            qid,
                            error,
                        ),
                    }
                }
            }
        }
        if *module_parameters::g17p_render_seed_ts.value() != 0 {
            if let Some(registers) = self.registers.as_ref() {
                for qid in [
                    queue_pair.tiling as u8,
                    queue_pair.fragment as u8,
                ] {
                    let seed =
                        u64::from(*module_parameters::g17p_render_seed_ts.value()) - 1;
                    match registers.g17p_seed_queue_completed_timestamp(qid, seed) {
                        Ok(()) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render: seeded qid {} completed-timestamp to {}\n",
                            qid,
                            seed,
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render: seeding qid {} failed ({:?})\n",
                            qid,
                            error,
                        ),
                    }
                }
            }
        }
        if (native_doorbell_mode == 2 || native_doorbell_mode == 3)
            && *module_parameters::g17p_render_slot_template.value() != 0
        {
            if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                .send_message(
                    g17_rtkit::ENDPOINT_GFX_INTERRUPTS,
                    g17_submission::G17P_NATIVE_3D_DOORBELL,
                )
            {
                return match g17_lifecycle::recover_persistent_runtime(self) {
                    Ok(()) => Err(error),
                    Err(recovery_error) => Err(recovery_error),
                };
            }
        }
        if native_doorbell_mode == 2 || native_doorbell_mode == 3 {
            if native_doorbell_mode == 3 {
                if let Err(error) = self
                    .manager
                    .as_mut()
                    .ok_or(ENODEV)?
                    .wait_native_render_fragment_outer_consumed(
                        prepared.fragment.next_producer,
                    )
                {
                    return match g17_lifecycle::recover_persistent_runtime(self) {
                        Ok(()) => Err(error),
                        Err(recovery_error) => Err(recovery_error),
                    };
                }

                if *module_parameters::g17p_render_ksm_probe.value() != 0 {
                    let registers = self.registers.as_ref().ok_or(ENODEV)?;
                    match registers.g17p_render_fault_irq_snapshot() {
                        Ok(Some(snapshot)) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P fault IRQ @render-post-fragment-pre-ta: gpc-state={:#x} info={:#018x} addr-word={:#018x} gate={:#018x} sub-status={:#018x} irq-status={:#018x} bit35={}\n",
                            snapshot.gpc_state,
                            snapshot.fault_info,
                            snapshot.fault_addr_word,
                            snapshot.requestor_gate,
                            snapshot.sub_status,
                            snapshot.irq_status,
                            (snapshot.irq_status >> 35) & 1,
                        ),
                        Ok(None) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P fault IRQ @render-post-fragment-pre-ta: skipped because gpc-state=0\n"
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P fault IRQ @render-post-fragment-pre-ta: snapshot failed ({:?})\n",
                            error,
                        ),
                    }
                    if let Err(error) = registers
                        .g17p_log_fault_requestors_pre_ack("render-post-fragment-pre-ta")
                    {
                        dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P fault requestor @render-post-fragment-pre-ta: sweep failed ({:?})\n",
                            error,
                        );
                    }
                }
            }
            let registers = self.registers.as_ref().ok_or(ENODEV)?;
            if let Err(error) = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .publish_native_render_tiling_group(&drm, registers, &prepared.tiling)
            {
                return match g17_lifecycle::recover_persistent_runtime(self) {
                    Ok(()) => Err(error),
                    Err(recovery_error) => Err(recovery_error),
                };
            }

            if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                .send_message(
                    g17_rtkit::ENDPOINT_GFX_INTERRUPTS,
                    g17_submission::G17P_NATIVE_TA_DOORBELL,
                )
            {
                return match g17_lifecycle::recover_persistent_runtime(self) {
                    Ok(()) => Err(error),
                    Err(recovery_error) => Err(recovery_error),
                };
            }
            if *module_parameters::g17p_irq_burst.value() != 0 {
                if let Some(registers) = self.registers.as_ref() {
                    let mut seen = [(0u64, 0u64, 0u64); 12];
                    match registers.g17p_burst_sample_irq(&mut seen) {
                        Ok(n) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render IRQBURST distinct={} [{:#x}/{:#x}/{:#x}] [{:#x}/{:#x}/{:#x}] [{:#x}/{:#x}/{:#x}] [{:#x}/{:#x}/{:#x}]\n",
                            n,
                            seen[0].0, seen[0].1, seen[0].2,
                            seen[1].0, seen[1].1, seen[1].2,
                            seen[2].0, seen[2].1, seen[2].2,
                            seen[3].0, seen[3].1, seen[3].2,
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render IRQBURST failed ({:?})\n",
                            error,
                        ),
                    }
                }
            }
            if *module_parameters::g17p_slot_burst.value() != 0 {
                if let Some(registers) = self.registers.as_ref() {
                    let mut seen = [0u64; 16];
                    match registers.g17p_burst_sample_slot0(&mut seen) {
                        Ok(n) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render SLOTBURST distinct={} [{:#018x},{:#018x},{:#018x},{:#018x},{:#018x},{:#018x},{:#018x},{:#018x}]\n",
                            n,
                            seen[0], seen[1], seen[2], seen[3],
                            seen[4], seen[5], seen[6], seen[7],
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P render SLOTBURST failed ({:?})\n",
                            error,
                        ),
                    }
                }
            }
            if *module_parameters::g17p_dep_records.value() != 0 {
                if let Some(manager) = self.manager.as_mut() {
                    manager.log_render_outer_channels("post-doorbells");
                }
            }
            if *module_parameters::g17p_scan_gate.value() != 0 {
                let polls = 64usize;
                for (label, link, mask) in [
                    ("run-state 0x1771f8", 0xffff_fc00_0017_71f8u64, 0x78u64),
                    ("fwdata+0x124ba0", 0xffff_fc00_0012_4ba0u64, 0xffff_ffff_0000_0000u64),
                    ("wakeup 0x177190", 0xffff_fc00_0017_7190u64, 0x7u64),
                ] {
                    let phys = g17_manager::g17p_gfx_link_to_physical(link);
                    match crate::pgtable::LiveFirmwareU64Probe::new(phys)
                        .and_then(|p| p.observe_mask(mask, polls))
                    {
                        Ok(o) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P scan-gate[{}]: first={:#018x} last={:#018x} or={:#018x} mask={:#x} seen={} polls={}\n",
                            label,
                            o.first,
                            o.last,
                            o.observed_or,
                            mask,
                            o.mask_seen,
                            o.polls,
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P scan-gate[{}]: read failed ({:?})\n",
                            label,
                            error,
                        ),
                    }
                }
            }
            let dump = *module_parameters::g17p_reg_dump.value();
            if dump != 0 {
                if let Some(registers) = self.registers.as_ref() {
                    let base = (dump as usize) << 8;
                    let mut words = [0u32; 64];
                    match registers.g17p_dump_sgx_window(base, &mut words) {
                        Ok(()) => {
                            for row in 0..16usize {
                                dev_info!(
                                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                    "G17P regdump {:#08x}: {:08x} {:08x} {:08x} {:08x}\n",
                                    base + row * 16,
                                    words[row * 4],
                                    words[row * 4 + 1],
                                    words[row * 4 + 2],
                                    words[row * 4 + 3],
                                );
                            }
                        }
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P regdump {:#08x}: read failed ({:?})\n",
                            base,
                            error,
                        ),
                    }
                }
            }
            self.probe_render_ksm_admission_after_render_doorbells();
            if *module_parameters::g17p_scan_gate.value() != 0 {
                if let Some(manager) = self.manager.as_mut() {
                    manager.log_render_outer_channels("post-wait");
                }
                for (label, link) in [
                    ("recovery-open 0x1771e8", 0xffff_fc00_0017_71e8u64),
                    ("event-stash 0x1771e0", 0xffff_fc00_0017_71e0u64),
                ] {
                    let phys = g17_manager::g17p_gfx_link_to_physical(link);
                    match crate::pgtable::LiveFirmwareU64Probe::new(phys)
                        .and_then(|p| p.observe_mask(!0u64, 2))
                    {
                        Ok(o) => dev_info!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P scan-late[{}]: {:#018x}\n",
                            label,
                            o.last,
                        ),
                        Err(error) => dev_warn!(
                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                            "G17P scan-late[{}]: read failed ({:?})\n",
                            label,
                            error,
                        ),
                    }
                }
            }
        } else {
            if native_doorbells {
                for doorbell in [
                    g17_submission::G17P_NATIVE_3D_DOORBELL,
                    g17_submission::G17P_NATIVE_TA_DOORBELL,
                ] {
                    if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                        .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, doorbell)
                    {
                        return match g17_lifecycle::recover_persistent_runtime(self) {
                            Ok(()) => Err(error),
                            Err(recovery_error) => Err(recovery_error),
                        };
                    }
                }
            } else {
                // Keep the producer store(s) and the initial work doorbell
                // adjacent. Default publishes the complete pair; split launch
                // publishes only TA and retains Fragment until TA completion.
                let publish = match self.manager.as_mut() {
                    Some(manager) if split_launch => manager
                        .publish_split_render_tiling_after_freelist(&prepared.tiling),
                    Some(manager) => {
                        manager.publish_default_render_pair_after_freelist(&prepared)
                    }
                    None => Err(ENODEV),
                };
                if let Err(error) = publish {
                    return match g17_lifecycle::recover_persistent_runtime(self) {
                        Ok(()) => Err(error),
                        Err(recovery_error) => Err(recovery_error),
                    };
                }
                fence(Ordering::SeqCst);
                let send = match self.gfx_rtkit.as_mut() {
                    Some(rtkit) => Pin::new(rtkit).send_message(
                        g17_rtkit::ENDPOINT_GFX_INTERRUPTS,
                        prepared.paired_doorbell,
                    ),
                    None => Err(ENODEV),
                };
                if let Err(error) = send {
                    return match g17_lifecycle::recover_persistent_runtime(self) {
                        Ok(()) => Err(error),
                        Err(recovery_error) => Err(recovery_error),
                    };
                }
                if let Some(pdev) = self.pdev.as_ref() {
                    dev_info!(
                        pdev.as_ref(),
                        "G17P render: deferred {} outer publication completed after USC FList retirement and initial doorbell sent\n",
                        if split_launch { "TA-only" } else { "paired" },
                    );
                }
            }
            self.probe_render_ksm_admission_after_render_doorbells();
        }
        if *module_parameters::g17p_power_wire.value() >= 2 {
            if let Err(error) = self.wire_g17p_submit_gpu_power() {
                return match g17_lifecycle::recover_persistent_runtime(self) {
                    Ok(()) => Err(error),
                    Err(recovery_error) => Err(recovery_error),
                };
            }
        }
        // gpc-state probe -- OPT-IN, and deliberately confined to the window
        // immediately after the doorbell.
        //
        // `log_gate` reads the sgx register file (regs.rs
        // `g17p_scheduler_gate_registers` -> `self.sgx.try_access()`). On this
        // chip an sgx read taken with the GPU cores power-gated raises an
        // asynchronous SError and panics the machine, which rules out the two
        // points a probe would otherwise naturally go:
        //
        //   * before publish -- `render-pre-doorbell` is DRAM-only precisely
        //     because the cores may still be gated there;
        //   * at the wait timeout -- by then the cores have powered back down.
        //     `abandon_compute_submission` records that doing this panicked the
        //     box on every timeout it was tried on.
        //
        // The compute path's one sample is safe only because it runs after a
        // completion woke the caller, so the GPU is provably alive. Render never
        // reaches that moment, so the closest safe approximation is a dense
        // burst right after the kick, while the firmware is still handling it.
        // Compute's signature is a transient 0x0 -> 0x1 -> 0x0, which a single
        // read would very likely miss -- hence a burst rather than one sample.
        let gate_probe = *module_parameters::g17p_render_gate_probe.value();
        if gate_probe != 0 && split_launch {
            if let Some(pdev) = self.pdev.as_ref() {
                dev_warn!(
                    pdev.as_ref(),
                    "G17P render split-launch: suppressing the SGX gate probe so the TA timestamp gate stays below the recovery window\n"
                );
            }
        } else if gate_probe != 0 {
            let samples = if gate_probe > 1 { 40 } else { 12 };
            for _ in 0..samples {
                self.log_gate("render-post-doorbell");
                fsleep(Delta::from_millis(1));
            }
        }
        // Do not scan the multi-MiB TVB/RT allocations while the render is
        // running. This call owns the runtime mutex, so that scan also delays
        // the firmware-event worker; observed scans exceeded the firmware's
        // watchdog interval before the first inline event service. Preserve
        // full memory dumps at recovery/failure or after completion instead.
        if split_launch {
            let tiling_timestamps = match self.wait_split_render_tiling_completion() {
                Ok(timestamps) => timestamps,
                Err(error) => {
                    return match g17_lifecycle::recover_persistent_runtime(self) {
                        Ok(()) => Err(error),
                        Err(recovery_error) => Err(recovery_error),
                    }
                }
            };
            if let Some(pdev) = self.pdev.as_ref() {
                dev_info!(
                    pdev.as_ref(),
                    "G17P render split-launch: TA completed [{:#x},{:#x}]; publishing deferred 3D outer producer\n",
                    tiling_timestamps[0],
                    tiling_timestamps[1]
                );
            }
            let registers = self.registers.as_ref().ok_or(ENODEV)?;
            if let Err(error) = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .publish_split_render_fragment_group(&drm, registers, &prepared.fragment)
            {
                // See the completion-wait path: a post-publication render
                // failure must not reboot the firmware CPUs. That reboot times
                // out and the failure path leaves the session refusing every
                // later submission.
                self.abandon_render_submission_after_timeout();
                return Err(error);
            }
            if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, prepared.paired_doorbell)
            {
                // See the completion-wait path: a post-publication render
                // failure must not reboot the firmware CPUs. That reboot times
                // out and the failure path leaves the session refusing every
                // later submission.
                self.abandon_render_submission_after_timeout();
                return Err(error);
            }
            // As for paired publication, enter the servicing wait before any
            // expensive host-memory dump of the now-live fragment resources.
        }
        if defer_completion {
            return Ok(());
        }
        let completion = match self.wait_render_completion_servicing_events() {
            Ok(completion) => completion,
            Err(error) => {
                if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
                    log_g17p_status_a(pdev.as_ref(), manager, "render-wait-failed");
                    // 0x40 tag15-programmed, 0x41 tag14-addkicks, 0x46
                    // pause-reason: the three codes that diagnosed compute.
                    probe_g17p_ktrace(pdev.as_ref(), manager, "render-wait-failed");
                    probe_g17p_reports(pdev.as_ref(), manager, "render-wait-failed");

                    dump_g17p_firmware_log(pdev.as_ref(), manager, "render-wait-failed");
                }
                if let Some(manager) = self.manager.as_mut() {
                    manager.log_render_status_pages("wait-failed");
                    manager.log_render_sksm_entries("wait-failed");
                    // Render completes into ordinal 2, which nothing had ever
                    // scanned. DRAM only, so safe on the timeout path.
                    let _ = manager.dump_completion_records("render-wait-failed");
                }
                // A render that does not complete is NOT a reason to tear the
                // runtime down. `recover_persistent_runtime` stops and reboots
                // both firmware CPUs; on this part that reboot regularly times
                // out in apple_rtkit_boot (~1s), and the failure path then
                // leaves `accepting_submissions` false forever, so every later
                // submission -- render OR compute, from any client -- returns
                // ENOTSUPP. One unfinished render bricked the device for the
                // rest of the boot.
                //
                // The firmware is demonstrably alive here: it has already
                // raised and serviced its own stamp-blamed recovery, answers
                // KSM register reads and keeps draining its rings. So release
                // this submission's own state and stay open for business. A
                // genuine crash still reaches the full path through
                // `is_crashed()` on the next submit.
                self.abandon_render_submission_after_timeout();
                return Err(error);
            }
        };
        if let (Some(pdev), Some(manager)) = (self.pdev.as_ref(), self.manager.as_mut()) {
            log_g17p_status_a(pdev.as_ref(), manager, "render-post-wait");
            probe_g17p_ktrace(pdev.as_ref(), manager, "render-post-wait");
            dump_g17p_firmware_log(pdev.as_ref(), manager, "render-post-wait");
            manager.log_render_status_pages("post-completion");
        }
        if !completion.complete {
            return match g17_lifecycle::recover_persistent_runtime(self) {
                Ok(()) => Err(EIO),
                Err(recovery_error) => Err(recovery_error),
            };
        }
        Ok(())
    }

    /// Publish one translated compute command and wait for its execution
    /// timestamps. The direct first-bind path retains the mapping and VM bind
    /// beyond this return until correlated ordinal-0 completion or stopped-CPU
    /// recovery, so timestamp visibility alone cannot release firmware state.
    /// Drive any open firmware recovery to completion before publishing.
    ///
    /// Returns once the shared handshake state reads 0. Answering every
    /// recovery is mandatory -- a capped or skipped answer leaves the KSM
    /// front end halted forever, because the host write of 2 is what makes the
    /// firmware schedule the unhalt.
    fn settle_open_recovery_before_publication(&mut self) -> Result {
        const SETTLE_POLLS: usize = 200;
        for attempt in 0..SETTLE_POLLS {
            let state = self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .firmware_recovery_handshake_snapshot()?
                .state;
            if state == 0 {
                if attempt != 0 {
                    dev_info!(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        "G17P submit: recovery settled after {} polls\n",
                        attempt
                    );
                }
                return Ok(());
            }
            let _ = self.service_firmware_events_inline();
            fsleep(Delta::from_millis(1));
        }
        dev_err!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P submit: firmware recovery still open after {} polls; refusing to publish\n",
            SETTLE_POLLS
        );
        Err(EBUSY)
    }

    fn wire_g17p_submit_gpu_power(&mut self) -> Result {
        let pdev = self.pdev.as_ref().cloned().ok_or(ENODEV)?;
        let publication = match self
            .manager
            .as_mut()
            .ok_or(ENODEV)?
            .publish_primary_device_control_record(
                g17_initdata::DEVICE_CONTROL_OPCODE_IDLE_POWER_OFF,
                g17_initdata::DEVICE_CONTROL_IDLE_POWER_OFF_ARG_POWER_ON,
            ) {
            Ok(publication) => publication,
            Err(error) => {
                dev_warn!(
                    pdev.as_ref(),
                    "G17P submit power: IdlePowerOff(0) record not staged ({:?}); submitting with the cores as they are\n",
                    error
                );
                return Ok(());
            }
        };
        fence(Ordering::SeqCst);
        let announcement = g17_rtkit::encode_primary_device_control();
        dev_info!(
            pdev.as_ref(),
            "G17P submit power: IdlePowerOff(0) staged opcode={:#x} arg={} at {:#x} slot={} producer {}->{} consumer={}; announcing {:#x}\n",
            publication.opcode,
            publication.arg,
            publication.record_address,
            publication.slot,
            publication.producer_before,
            publication.producer_after,
            publication.consumer_before,
            announcement
        );
        if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
            .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, announcement)
        {
            dev_warn!(
                pdev.as_ref(),
                "G17P submit power: device-control announcement failed ({:?}); the record stays staged for the next drain\n",
                error
            );
            return Ok(());
        }
        // The power-on above leaves `[0x177838]` at 0, which is one of the
        // guards the scheduler's idle-off routine (`0x7ca0`) tests: each guard
        // branches to `0x871c` = `mov w0,#1; ret` when set, an early return
        // that skips the bit-1 acquire at `0x8228` -- i.e. skips closing the
        // core power gate. So publish a second record with a non-zero arg to
        // hold the gate open across the submit. It cannot be one record: the
        // handler skips the power-on whenever the arg is non-zero.
        if *module_parameters::g17p_power_hold.value() != 0 {
            match self
                .manager
                .as_mut()
                .ok_or(ENODEV)?
                .publish_primary_device_control_record(
                    g17_initdata::DEVICE_CONTROL_OPCODE_IDLE_POWER_OFF,
                    g17_initdata::DEVICE_CONTROL_IDLE_POWER_OFF_ARG_INHIBIT,
                ) {
                Ok(hold) => {
                    fence(Ordering::SeqCst);
                    dev_info!(
                        pdev.as_ref(),
                        "G17P submit power: idle-off inhibit staged arg={} slot={} producer {}->{}\n",
                        hold.arg,
                        hold.slot,
                        hold.producer_before,
                        hold.producer_after
                    );
                    if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                        .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, announcement)
                    {
                        dev_warn!(
                            pdev.as_ref(),
                            "G17P submit power: inhibit announcement failed ({:?})\n",
                            error
                        );
                    }
                }
                Err(error) => dev_warn!(
                    pdev.as_ref(),
                    "G17P submit power: idle-off inhibit not staged ({:?})\n",
                    error
                ),
            }
        }
        for attempt in 0..8 {
            fsleep(Delta::from_millis(1));
            if let Ok(counters) = self.manager.as_mut().ok_or(ENODEV)?.control_counters() {
                dev_info!(
                    pdev.as_ref(),
                    "G17P submit power: control cursors after {}ms primary={:?} secondary={:?}\n",
                    attempt + 1,
                    counters.primary,
                    counters.secondary
                );
                if counters.primary[0] == counters.primary[2] {
                    break;
                }
            }
        }
        let mut gpc_state = 0u32;
        let mut powered = false;
        for _ in 0..G17P_POWER_WIRE_POLLS {
            match self
                .registers
                .as_ref()
                .ok_or(ENODEV)?
                .g17p_scheduler_gate_registers()
            {
                Ok(snapshot) => {
                    // Already masked to the low four bits by the accessor.
                    gpc_state = snapshot.host_irq_summary;
                    if gpc_state != 0 {
                        powered = true;
                        break;
                    }
                }
                Err(error) => {
                    dev_warn!(
                        pdev.as_ref(),
                        "G17P submit power: gpc-state read failed ({:?})\n",
                        error
                    );
                    break;
                }
            }
            fsleep(Delta::from_millis(1));
        }
        if powered {
            dev_info!(
                pdev.as_ref(),
                "G17P submit power: gpc-state={:#x} after IdlePowerOff(0); cores report powered (ceiling 10)\n",
                gpc_state
            );
        } else {
            dev_warn!(
                pdev.as_ref(),
                "G17P submit power: gpc-state still {:#x} after {} polls of IdlePowerOff(0)\n",
                gpc_state,
                G17P_POWER_WIRE_POLLS
            );
        }
        {
            let manager = self.manager.as_mut().ok_or(ENODEV)?;
            probe_g17p_ktrace(pdev.as_ref(), manager, "post-idle-power-off");
        }
        Ok(())
    }

    /// Publish one translated compute command on behalf of DRM queue `owner`.
    ///
    /// `owner` is the driver-side id of the client queue object. It is what
    /// makes the retained graph *per client*: the steady-state fast path below
    /// is taken only when the same client resubmits, and any other client goes
    /// through a recycle first.
    pub(crate) fn submit_translated_compute(
        &mut self,
        owner: u64,
        vm: &mmu::Vm,
        command: &g17_uapi::TranslatedComputeCommand,
        owned_timestamps: mmu::KernelMapping,
    ) -> Result<G17PSubmitOutcome> {
        if !self.test_stage.enables_compute_uapi() {
            return Err(ENOTSUPP);
        }
        if !self.accepting_submissions {
            return Err(ENOTSUPP);
        }
        self.settle_open_recovery_before_publication()?;
        // Compute owns disjoint QID4+ state and an independent application-
        // GART root.  Render's two job-private roots may stay live; reclaiming
        // either here would turn this publication back into a cross-engine
        // scheduling barrier.
        self.manager.as_mut().ok_or(ENODEV)?.stage_compute_flist_state()?;
        // Allocate the compute graph on actual compute use, never as a
        // prerequisite for render. This is a no-op once a graph exists.
        self.ensure_compute_queue()?;
        if !self.accepting_submissions {
            return Err(ENOTSUPP);
        }
        // Hand the graph over if a different client owned it last. This is the
        // step that makes a second process work at all: without it the retained
        // binding of a client that has already exited still pinned UAT slot 1,
        // and the new client's first submit tore down the whole session.
        self.acquire_compute_owner(owner)?;
        // A handoff may have rebuilt the whole session; re-read the gate.
        if !self.accepting_submissions {
            return Err(ENOTSUPP);
        }
        // QUIESCE the firmware after a closed gate, before publishing anything.
        //
        // FW RE (b1): pause bit 3 has exactly ONE clear site image-wide --
        // `0x25dd8`, inside the scheduler thread's idle descent, reached only
        // from `0x25c00` and the `cbnz -> 0x25dc0` routes at `0x25a04`/
        // `0x25a50`/`0x25a70`. The generic pause helper `0xa1c4(set, bit)` is
        // never called with bit 8 by anything in the image, so no host message
        // can clear it directly. But the descent's own guards at `0x25940` are
        // queue state we DO drive: it descends only when the producer at
        // `[x8+416]` equals the consumer at `[x8+432]`, none of mask
        // `0x8_032c_0007` is set in `[x19]`, and `[x19+96]` is zero. Submitting
        // continuously is what keeps the thread on the work path at `0x25e2c`,
        // which is why no 0x46 record appears after a fault at all.
        //
        // So: go quiet and let the thread reach a genuine idle. Its descent is
        // idempotent on an already-set bit 3, and its EXIT clears bit 3 --
        // leaving mask 0x2, from which the existing IdlePowerOff(0) lever
        // (`g17p_power_wire`, which reaches `0x21960`) is the clear that takes
        // the mask to zero and therefore actually invokes `0x20a70`.
        // Pair this with `g17p_power_wire=2`.
        if self.compute_gate_suspect {
            let quiesce = *module_parameters::g17p_gate_quiesce_ms.value();
            self.compute_gate_suspect = false;
            if quiesce != 0 {
                if let Some(pdev) = self.pdev.as_ref() {
                    dev_info!(
                        pdev.as_ref(),
                        "G17PMARK gate-quiesce {}ms before publishing; letting the scheduler reach a genuine idle so its exit can clear pause bit 3\n",
                        quiesce
                    );
                }
                fsleep(Delta::from_millis(quiesce as i64));
            }
        }
        // Same-owner retry after one of this client's own submissions was
        // abandoned. No handoff ran, so nothing has repaired the accounting
        // the abandoned kick leaked -- and `producer.submitted` is capped at
        // the geometry's 0x100 stamps, so a client that retries after failures
        // would otherwise wedge its own queue. Its binding is still live and
        // still owned by this client, so nothing needs quarantining here.
        if self.compute_abandoned
            && *module_parameters::g17p_abandon_recovery.value() != 0
            && !self.is_crashed()
        {
            if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
                manager.resynchronise_compute_queue_after_abandon(queue)?;
            }
            self.compute_abandoned = false;
        }
        // A repeat submission on a live queue must NOT rebuild the session.
        // The retained binding and timestamp mapping left behind by the last
        // submission are exactly what makes a steady-state resubmit possible;
        // tearing the firmware session down and re-running the first-bind
        // chronology instead is the behaviour that made repeats fail. Reuse
        // the binding when the queue is installed and the VM is unchanged.
        let steady_state_queue = !self.is_crashed()
            && self
                .queue
                .as_ref()
                .is_some_and(|queue| self.manager.as_ref().is_some_and(|manager| {
                    manager.compute_queue_is_installed(queue)
                }));
        let repeat_ready = steady_state_queue
            && self.compute_binding.as_ref().is_some_and(|binding| binding.matches(vm));
        // A handoff (`rebind_compute_queue`) released the previous owner's
        // binding and KEPT the installed QID 4 graph. This submission must
        // therefore bind the incoming VM and then take the SAME steady-state
        // chronology a repeat takes -- never the first-bind one, whose tag-15
        // record carries the queue-install flag and would make the firmware
        // disable/reprogram/enable a KSMFE queue it already has installed. The
        // manager routes on the queue alone (`hardware_state` plus
        // `submission_ordinal`), so retaining the queue is what selects the
        // right path; all this flag does is stop the host-side first-bind
        // preconditions from rejecting it.
        let rebind_ready =
            steady_state_queue && !repeat_ready && self.compute_binding.is_none();
        if repeat_ready || rebind_ready {
            // Only the previous submission's owned timestamp page is dropped;
            // on a repeat the user-VM binding stays, and on a rebind it is
            // already gone.
            drop(self.compute_timestamps.take());
        } else if self.is_crashed()
            || self.compute_binding.is_some()
            || self.compute_timestamps.is_some()
        {
            g17_lifecycle::recover_persistent_runtime(self)?;
        }
        self.settle_open_recovery_before_publication()?;
        // Any recovery above rebuilt the session, and a rebuilt session comes
        // back with no queue: `boot_session` defers QID 4 to the event worker.
        // Put one back synchronously, or every clause below reads `None` and
        // the submit fails ENODEV for a reason that has nothing to do with it.
        self.manager.as_mut().ok_or(ENODEV)?.stage_compute_flist_state()?;
        self.ensure_compute_queue()?;
        // The prepublication contract describes a pristine queue -- ordinal 0,
        // seed timestamp, nothing outstanding -- so it only applies to a first
        // bind. A steady-state queue legitimately violates every clause, and a
        // queue handed to a new client through `rebind_compute_queue` is a
        // steady-state queue: it keeps its ordinal, its stamp sequence and its
        // ring cursor precisely so the firmware never sees the install flag a
        // second time.
        if !repeat_ready && !rebind_ready {
            self.manager
                .as_ref()
                .ok_or(ENODEV)?
                .validate_prepublication_compute_queue(self.queue.as_ref().ok_or(ENODEV)?)?;
        }
        if g17_submission::require_g17p_running_client_power_transition(
            g17_submission::G17P_RUNNING_CLIENT_POWER_CONTRACT,
            self.cpus.is_some(),
            self.gfx1_rtkit.is_some(),
        )
        .is_err()
        {
            dev_err!(
                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                "G17P compute power: retained target1 CPU/RTKit owner missing before VM bind\n"
            );
            return Err(ENOTSUPP);
        }
        // The one host lever that releases KSM pause reason bit 1.
        //
        // ORDER IS LOAD-BEARING when the mask is 0xa (bits 3|1). FW RE of b1:
        // every pause-reason clear site -- bit 1 at 0xe488, bit 3 at 0x25dd4 --
        // is `ands` followed by `b.ne <skip>`, so the core power sequencer
        // 0x20a70 is invoked ONLY on the transition to mask == 0. Clearing bit
        // 1 while bit 3 is still set therefore cannot re-power the cores by
        // construction, which is exactly why this lever measured as a no-op.
        // Mode 2 defers it until AFTER the kick is published, so the scheduler
        // has a chance to clear bit 3 first (0xdeb8 reports outstanding work ->
        // 0x25c00 -> 0x25dc0) and this record's bit-1 clear becomes the
        // transition to zero.
        if *module_parameters::g17p_power_wire.value() == 1 {
            self.wire_g17p_submit_gpu_power()?;
        }
        {
            // Checkpoint B: the opening control handshake has retired by now,
            // so an armed KTrace must already have moved the producer even
            // though nothing has been submitted yet.
            let pdev_probe = self.pdev.as_ref().cloned().ok_or(ENODEV)?;
            let manager = self.manager.as_mut().ok_or(ENODEV)?;
            probe_g17p_ktrace(pdev_probe.as_ref(), manager, "pre-first-submit");
        }
        if owned_timestamps.size() < 16 {
            return Err(ERANGE);
        }
        let timestamp_start = owned_timestamps.iova();
        let timestamp_end = timestamp_start.checked_add(8).ok_or(EOVERFLOW)?;
        if !repeat_ready {
            let binding = {
                let manager = self.manager.as_mut().ok_or(ENODEV)?;
                let queue = self.queue.as_mut().ok_or(ENODEV)?;
                manager.bind_compute_user_vm(queue, vm)?
            };
            self.compute_binding = Some(binding);
        }
        let submit_result = {
            let registers = self.registers.as_ref().ok_or(ENODEV)?;
            let manager = self.manager.as_mut().ok_or(ENODEV)?;
            let queue = self.queue.as_mut().ok_or(ENODEV)?;
            let binding = self.compute_binding.as_ref().ok_or(ENODEV)?;
            let gfx_state = self.gfx_state.as_ref().ok_or(ENODEV)?;
            gfx_state
                .submission_phase
                .store(g17_manager::G17P_SUBMIT_PHASE_IDLE, Ordering::Release);
            manager.submit_translated_compute(
                registers,
                queue,
                &gfx_state.submission_phase,
                binding,
                command,
                g17_manager::G17PComputeSubmissionContext {
                    context_id: 2,
                    dispatch_identity: 0x0100_01d7_0200_01dc,
                    execution_gate: 1,
                    timestamps: g17_uapi::ComputeTimestampAddresses {
                        start: timestamp_start,
                        end: timestamp_end,
                    },
                    descriptor_flag_4c: false,
                    descriptor_flag_5c8: false,
                    converted_command_timestamp: 1,
                    barriers: &[],
                    mcache: None,
                    rce_kind: 0,
                    auxiliary: 0x003f_ffff_ffff_ffff,
                },
            )
        };
        let notifications = match submit_result {
            Ok(notifications) => notifications,
            Err(error) => {
                let phase = self
                    .gfx_state
                    .as_ref()
                    .map(|state| state.submission_phase.load(Ordering::Acquire))
                    .unwrap_or(g17_manager::G17P_SUBMIT_PHASE_IDLE);
                dev_err!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P compute: host submission failed before notification phase={} err={:?}\n",
                    phase,
                    error
                );
                // See the completion-wait path: a post-publication render
                // failure must not reboot the firmware CPUs. That reboot times
                // out and the failure path leaves the session refusing every
                // later submission.
                self.abandon_render_submission_after_timeout();
                return Err(error);
            }
        };
        let channel_path = notifications.is_some();
        if let Some(notifications) = notifications {
            if let Some(activation) = notifications.activation {
                log_g17p_scheduler_gate(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.registers.as_ref().ok_or(ENODEV)?,
                    "native-first-pre-0x0a",
                );
                log_g17p_status_a(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-pre-0x0a",
                );
                log_g17p_firmware_recovery_handshake(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-pre-0x0a",
                );
                log_g17p_fault_report(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-pre-0x0a",
                );
                log_g17p_a2i_control(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.cpus.as_ref().ok_or(ENODEV)?.gfx(),
                    "native-first-pre-0x0a",
                );
                dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P compute: CL_2 published; sending priority-2 activation {:#x}\n",
                    activation
                );
                if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                    .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, activation)
                {
                    return match g17_lifecycle::recover_persistent_runtime(self) {
                        Ok(()) => Err(error),
                        Err(recovery_error) => Err(recovery_error),
                    };
                }
                log_g17p_a2i_control(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.cpus.as_ref().ok_or(ENODEV)?.gfx(),
                    "native-first-post-0x0a-pre-0x10",
                );
                log_g17p_status_a(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-post-0x0a-pre-0x10",
                );
            }
            if let Some(direct_kick) = notifications.direct_kick {
                fence(Ordering::SeqCst);
                dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P compute: direct EP kick {:#x} after activation\n",
                    direct_kick
                );
                if let Err(error) = Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                    .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, direct_kick)
                {
                    return match g17_lifecycle::recover_persistent_runtime(self) {
                        Ok(()) => Err(error),
                        Err(recovery_error) => Err(recovery_error),
                    };
                }
                log_g17p_scheduler_gate(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.registers.as_ref().ok_or(ENODEV)?,
                    "native-first-post-0x10",
                );
                log_g17p_a2i_control(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.cpus.as_ref().ok_or(ENODEV)?.gfx(),
                    "native-first-post-0x10",
                );
                log_g17p_status_a(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-post-0x10",
                );
                log_g17p_firmware_recovery_handshake(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-post-0x10",
                );
                log_g17p_fault_report(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    self.manager.as_mut().ok_or(ENODEV)?,
                    "native-first-post-0x10",
                );
            }
        }

        // Deferred power lever: the kick is published and AddKicks has been
        // seen, so `0xdeb8` can now report outstanding work and let the
        // scheduler's idle loop clear pause bit 3. Clearing bit 1 here is then
        // the clear that takes the mask to zero and calls the core power
        // sequencer.
        if *module_parameters::g17p_power_wire.value() >= 2 {
            if let Err(error) = self.wire_g17p_submit_gpu_power() {
                dev_warn!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P compute power: deferred power lever failed ({:?})\n",
                    error
                );
            }
        }
        let compute_queue_id = u64::from(
            self.manager
                .as_ref()
                .ok_or(ENODEV)?
                .compute_queue_id(self.queue.as_ref().ok_or(ENODEV)?)?,
        );
        // Control sample for the render blocker: compute reaches dispatch on
        // this same driver, so reading the KSM slot bank and the bit-4
        // new-work path right after a compute kick shows what a working
        // notification looks like. Same probe, same registers, same gate.
        self.probe_render_ksm_admission_after_render_doorbells();
        let mut scan_observation = G17PWorkScanObservation::new();
        let mut serviced_notifications: u32 = 0;
        let mut last_ksm_sample: (u64, u64, u64) = (0, 0, 0);
        let mut add_kicks_published = false;
        let pdev_poll = self.pdev.as_ref().cloned().ok_or(ENODEV)?;
        let mut service_failures: u32 = 0;
        let ktrace_enabled = g17p_ktrace_enabled();
        let ktrace_verbose = *module_parameters::g17p_fw_ktrace_verbose.value() != 0;
        let ktrace_budget = *module_parameters::g17p_fw_ktrace_budget.value();
        let mut ktrace_primary = G17PKtraceTotals::default();
        let mut ktrace_secondary = G17PKtraceTotals::default();
        let mut ktrace_failures: u32 = 0;
        if *module_parameters::g17p_irq_completion.value() != 0 {
            // Publish and return. The wait happens in the caller with the
            // runtime mutex released, so the RTKit doorbell's event worker can
            // actually run and signal us -- holding the mutex across a wait is
            // what forced the old 625 ms timestamp poll and its "inline event
            // service" workaround.
            self.completion_signal.arm();
            self.pending_compute = Some((channel_path, owned_timestamps));
            return Ok(G17PSubmitOutcome::Pending(self.completion_signal()));
        }
        let completion = owned_timestamps.with_cpu_bytes(|raw| {
            let polls = *module_parameters::g17p_submit_polls.value() as usize;
            let mut low_probe_done = *module_parameters::g17p_probe_low_range.value() == 0;
            for poll in 0..polls {
                if *module_parameters::g17p_probe_low_range.value() != 0
                    && low_probe_done
                    && poll >= 50
                    && poll % 50 == 0
                {
                    if let Some(registers) = self.registers.as_ref() {
                        if let Ok(sample) = registers.g17p_sample_ksm(compute_queue_id) {
                            if sample != last_ksm_sample {
                                last_ksm_sample = sample;
                                dev_info!(
                                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                    "G17P KSM sample poll={} descriptor={:#x} pause={:#x} addkicks={:#x}\n",
                                    poll,
                                    sample.0,
                                    sample.1,
                                    sample.2
                                );
                            }
                        }
                    }
                }
                if !low_probe_done && poll >= 50 {
                    low_probe_done = true;
                    if let Some(registers) = self.registers.as_ref() {
                        match registers.g17p_probe_fault_info() {
                            Ok(value) => {
                                dev_info!(
                                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                    "G17P LOW RANGE REACHABLE while powered: 0x17030 = {:#x}\n",
                                    value
                                );
                                if let Err(error) = registers.g17p_probe_ksm_block() {
                                    dev_warn!(
                                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                        "G17P KSM block probe failed ({:?})\n",
                                        error
                                    );
                                }
                                if let Err(error) =
                                    registers.g17p_dump_queue_descriptor(compute_queue_id)
                                {
                                    dev_warn!(
                                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                        "G17P queue descriptor read failed ({:?})\n",
                                        error
                                    );
                                }
                                if *module_parameters::g17p_late_sksm.value() != 0 {
                                    match registers.g17p_replay_deferred_sksm() {
                                        Ok(issued) => dev_info!(
                                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                            "G17P SKSM replay issued {} pairs while powered\n",
                                            issued
                                        ),
                                        Err(error) => dev_warn!(
                                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                            "G17P SKSM replay failed ({:?})\n",
                                            error
                                        ),
                                    }
                                    if let Err(error) = registers.g17p_probe_ksm_block() {
                                        dev_warn!(
                                            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                            "G17P post-replay KSM probe failed ({:?})\n",
                                            error
                                        );
                                    }
                                }
                            }
                            Err(error) => dev_warn!(
                                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                                "G17P low-range probe failed ({:?})\n",
                                error
                            ),
                        }
                    }
                }
                match self.service_firmware_events_inline() {
                    Ok(count) => serviced_notifications += count,
                    Err(error) => {
                        // Log the first failure: swallowing it hid which of the
                        // drain's several `ok_or(ENODEV)` guards (or the batch
                        // drain itself) is refusing, on the one poll where the
                        // firmware actually had something for us.
                        if service_failures == 0 {
                            dev_warn!(
                                pdev_poll.as_ref(),
                                "G17P inline event service failed on poll {} ({:?})\n",
                                poll,
                                error
                            );
                        }
                        service_failures += 1;
                    }
                }
                // The submit path holds the runtime mutex for the whole
                // wait, which is why this drains here rather than from an
                // RTKit handler or a workqueue.
                if ktrace_enabled {
                    for role in [
                        g17_initdata::InstanceRole::Primary,
                        g17_initdata::InstanceRole::Secondary,
                    ] {
                        match self
                            .manager
                            .as_mut()
                            .ok_or(ENODEV)
                            .and_then(|manager| {
                                manager.drain_ktrace_ring(role, ktrace_verbose, ktrace_budget)
                            }) {
                            Ok(stats) => match role {
                                g17_initdata::InstanceRole::Primary => {
                                    ktrace_primary.accumulate(&stats)
                                }
                                g17_initdata::InstanceRole::Secondary => {
                                    ktrace_secondary.accumulate(&stats)
                                }
                            },
                            Err(_) => ktrace_failures += 1,
                        }
                    }
                }
                if !add_kicks_published
                    && self
                        .manager
                        .as_ref()
                        .is_some_and(|manager| manager.has_pending_add_kicks())
                {
                    let settled = self
                        .manager
                        .as_mut()
                        .ok_or(ENODEV)
                        .and_then(|manager| manager.firmware_recovery_handshake_snapshot())
                        .map(|handshake| handshake.state == 0)
                        .unwrap_or(false);
                    if settled {
                        add_kicks_published = true;
                        if let (Some(queue), Some(manager)) =
                            (self.queue.as_mut(), self.manager.as_mut())
                        {
                            if let Err(error) = manager.publish_pending_add_kicks(queue, 3) {
                                dev_warn!(
                                    pdev_poll.as_ref(),
                                    "G17P deferred AddKicks publish failed ({:?})\n",
                                    error
                                );
                            }
                            fence(Ordering::SeqCst);
                        }
                    }
                }
                match self
                    .manager
                    .as_mut()
                    .ok_or(ENODEV)
                    .and_then(|manager| manager.status_a_snapshot())
                {
                    Ok(snapshot) => scan_observation.observe(snapshot.scan_active),
                    Err(_) => scan_observation.observe_failure(),
                }
                fence(Ordering::Acquire);
                // The firmware reports completion in the KSM completion record,
                // not by writing the user timestamp pair: the descriptor's start
                // pointer is null, so `raw[0..8]` can never become non-zero.
                // Check the real record first.
                if let Some(manager) = self.manager.as_mut() {
                    if let Some(completion) = manager.read_compute_completion()? {
                        return Ok(completion);
                    }
                }
                let start = u64::from_le_bytes(raw[0..8].try_into().map_err(|_| ERANGE)?);
                let end = u64::from_le_bytes(raw[8..16].try_into().map_err(|_| ERANGE)?);
                if let Some(completion) = g17_uapi::completed_compute_timestamps(start, end) {
                    return Ok(completion);
                }
                fsleep(Delta::from_millis(1));
            }
            Err(ETIMEDOUT)
        });
        dev_info!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P Status-A scan observation first={} last={} or={} transitions={} samples={} failures={}\n",
            scan_observation.first,
            scan_observation.last,
            scan_observation.observed_or,
            scan_observation.transitions,
            scan_observation.samples,
            scan_observation.failures,
        );
        dev_info!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P inline event service: notifications={} failures={}\n",
            serviced_notifications,
            service_failures,
        );
        if *module_parameters::g17p_fw_log.value() != 0 {
            match self
                .manager
                .as_mut()
                .ok_or(ENODEV)
                .and_then(|manager| manager.dump_firmware_log(60))
            {
                Ok(count) => dev_info!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P fwlog: {} records\n",
                    count
                ),
                Err(error) => dev_warn!(
                    self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                    "G17P fwlog dump failed ({:?})\n",
                    error
                ),
            }
        }
        if ktrace_enabled {
            for role in [
                g17_initdata::InstanceRole::Primary,
                g17_initdata::InstanceRole::Secondary,
            ] {
                match self
                    .manager
                    .as_mut()
                    .ok_or(ENODEV)
                    .and_then(|manager| manager.drain_ktrace_ring(role, ktrace_verbose, 0))
                {
                    Ok(stats) => match role {
                        g17_initdata::InstanceRole::Primary => ktrace_primary.accumulate(&stats),
                        g17_initdata::InstanceRole::Secondary => {
                            ktrace_secondary.accumulate(&stats)
                        }
                    },
                    Err(_) => ktrace_failures += 1,
                }
            }
            dev_info!(
                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                "G17P ktrace summary: primary producer={} consumed={} printed={} skipped={} lost={}/{} addkicks={} pause-mask={:#x} | secondary producer={} consumed={} printed={} skipped={} lost={}/{} addkicks={} pause-mask={:#x} | failures={}\n",
                ktrace_primary.last_producer,
                ktrace_primary.consumed,
                ktrace_primary.printed,
                ktrace_primary.skipped,
                ktrace_primary.lost_records,
                ktrace_primary.lost_windows,
                ktrace_primary.saw_add_kicks,
                ktrace_primary.last_pause_mask,
                ktrace_secondary.last_producer,
                ktrace_secondary.consumed,
                ktrace_secondary.printed,
                ktrace_secondary.skipped,
                ktrace_secondary.lost_records,
                ktrace_secondary.lost_windows,
                ktrace_secondary.saw_add_kicks,
                ktrace_secondary.last_pause_mask,
                ktrace_failures,
            );
        }
        if let Some(manager) = self.manager.as_mut() {
            let _ = manager.dump_completion_stamps("post-submit");
        }
        let completion = match completion {
            Ok(completion) => completion,
            Err(error) => {
                if channel_path {
                    log_g17p_scheduler_gate(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        self.registers.as_ref().ok_or(ENODEV)?,
                        "native-first-timeout",
                    );
                    log_g17p_status_a(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        self.manager.as_mut().ok_or(ENODEV)?,
                        "native-first-timeout",
                    );
                    log_g17p_firmware_recovery_handshake(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        self.manager.as_mut().ok_or(ENODEV)?,
                        "native-first-timeout",
                    );
                    log_g17p_fault_report(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        self.manager.as_mut().ok_or(ENODEV)?,
                        "native-first-timeout",
                    );
                    log_g17p_a2i_control(
                        self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        self.cpus.as_ref().ok_or(ENODEV)?.gfx(),
                        "native-first-timeout",
                    );
                    let user_timestamps = owned_timestamps.with_cpu_bytes(|raw| {
                        Ok([
                            u64::from_le_bytes(raw[0..8].try_into().map_err(|_| ERANGE)?),
                            u64::from_le_bytes(raw[8..16].try_into().map_err(|_| ERANGE)?),
                        ])
                    });
                    let snapshot = {
                        let manager = self.manager.as_mut().ok_or(ENODEV)?;
                        let queue = self.queue.as_mut().ok_or(ENODEV)?;
                        manager.snapshot_compute_timeout(queue)
                    };
                    let pdev = self.pdev.as_ref().ok_or(ENODEV)?;
                    match (snapshot, user_timestamps) {
                        (Ok(snapshot), Ok(user_timestamps)) => {
                            let control = snapshot.control;
                            let channel = snapshot.channel;
                            let channel_scan = snapshot.channel_scan;
                            let channel_control_prefix = snapshot.channel_control_prefix;
                            let graph = snapshot.graph;
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: control primary={:?} secondary={:?}\n",
                                control.primary,
                                control.secondary
                            );
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: CL_2 table={} ring={:#x} state-ptrs(actual,mirror,producer)={:#x?} runtime-pointers(+120,+130)={:#x?} cursors(actual,mirror,producer)={:?} slot={} queue={:#x} kind={} flags={:#x} doorbell={:#x}\n",
                                channel.table_index,
                                channel.ring,
                                channel.state_addresses,
                                channel.runtime_descriptor_pointers,
                                channel.cursors,
                                channel.slot_index,
                                channel.slot_queue,
                                channel.slot_kind,
                                channel.slot_flags,
                                g17_submission::G17P_COMPUTE_WORK_DOORBELL
                            );
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: inner queue={:#x} pointers={:#x} ring={:#x} pointer[+0,+30,+40]={:?} queue[+18,+1c,+20]={:?} uuid={:#x} context={:#x}\n",
                                graph.queue,
                                graph.pointers,
                                graph.item_ring,
                                graph.cursors,
                                graph.gpu_read_cursors,
                                graph.uuid,
                                graph.queue_context
                            );
                            for scan in channel_scan {
                                dev_err!(
                                    pdev.as_ref(),
                                    "G17P compute timeout: scan table={} ring={:#x} state-ptrs(actual,mirror,producer)={:#x?} cursors(actual,mirror,producer)={:?} current-slot={} queue={:#x} kind={}\n",
                                    scan.table_index,
                                    scan.ring,
                                    scan.state_addresses,
                                    scan.cursors,
                                    scan.current_slot_index,
                                    scan.current_slot_queue,
                                    scan.current_slot_kind,
                                );
                            }
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: channel-control[0..0x38]={:x?}\n",
                                channel_control_prefix,
                            );
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: items={:#x?} event={:#x?} status={:#x?}\n",
                                graph.items,
                                graph.event,
                                graph.status
                            );
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: descriptor={:#x} selector={:#x} context={} grid={} timestamp-pointers={:#x?} values={:#x?}\n",
                                graph.descriptor,
                                graph.descriptor_selector,
                                graph.descriptor_context,
                                graph.descriptor_grid,
                                graph.descriptor_timestamps,
                                user_timestamps
                            );
                            dev_err!(
                                pdev.as_ref(),
                                "G17P compute timeout: optional-contexts={:#x?} grid={} uuid={:#x} shared={:#x} channel={:#x}; high-context={:#x} record={:#x?}\n",
                                graph.optional_contexts,
                                graph.optional_grid,
                                graph.optional_uuid,
                                graph.optional_shared,
                                graph.optional_channel,
                                graph.context_high,
                                graph.context_record
                            );
                        }
                        (Err(snapshot_error), _) => dev_err!(
                            pdev.as_ref(),
                            "G17P compute timeout snapshot failed ({:?})\n",
                            snapshot_error
                        ),
                        (_, Err(timestamp_error)) => dev_err!(
                            pdev.as_ref(),
                            "G17P compute timeout timestamp read failed ({:?})\n",
                            timestamp_error
                        ),
                    }
                }
                // See the completion-wait path: a post-publication render
                // failure must not reboot the firmware CPUs. That reboot times
                // out and the failure path leaves the session refusing every
                // later submission.
                self.abandon_render_submission_after_timeout();
                return Err(error);
            }
        };
        if !channel_path {
            let retire_result = self
                .manager
                .as_ref()
                .ok_or(ENODEV)?
                .complete_compute_entries(self.queue.as_mut().ok_or(ENODEV)?, 1);
            if let Err(error) = retire_result {
                // See the completion-wait path: a post-publication render
                // failure must not reboot the firmware CPUs. That reboot times
                // out and the failure path leaves the session refusing every
                // later submission.
                self.abandon_render_submission_after_timeout();
                return Err(error);
            }
        }
        if channel_path {
            self.compute_timestamps = Some(owned_timestamps);
            dev_info!(
                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                "G17P compute: first direct CL timestamps complete; retaining timestamp mapping, VM binding, and QID state until native ordinal-0 completion is wired\n"
            );
        } else {
            drop(self.compute_binding.take());
        }
        Ok(G17PSubmitOutcome::Completed(completion))
    }

    /// Complete a submission that was published with `Pending`.
    ///
    /// Runs with the runtime mutex re-acquired, after the caller has been woken
    /// by the firmware-event path. The completion itself is read from the KSM
    /// completion record, which is where the firmware actually reports finished
    /// work -- the firmware event ring carries telemetry, and the descriptor's
    /// user timestamp start pointer is null so that pair never becomes valid.
    pub(crate) fn finish_compute_submission(&mut self) -> Result<[u64; 2]> {
        let (channel_path, owned_timestamps) = self.pending_compute.take().ok_or(EINVAL)?;
        self.compute_abandoned = true;
        let completion = self
            .manager
            .as_mut()
            .ok_or(ENODEV)?
            .read_compute_completion()?
            .ok_or(ETIMEDOUT)?;
        // Consume it: KSM completion ordinal 0 is one overwritten slot, so
        // without this the record stays "complete" forever and the next
        // submission's wait would return instantly with these timestamps.
        self.manager
            .as_mut()
            .ok_or(ENODEV)?
            .commit_compute_completion(completion);
        // A retire whose start and end GPU timestamps are EQUAL means the
        // firmware retired work the GPU never executed. Measured on J700: a
        // healthy add3 retires with duration 0xda-0xdd ticks, and every
        // submission after a fault-blamed recovery retires with duration 0 and
        // an untouched destination buffer. Reporting that as success is the
        // worst outcome available -- it is a silent wrong answer, and for a
        // conformance run a false PASS is worse than a FAIL -- so record the
        // closed gate and, by default, fail the submission honestly.
        if completion[1] == completion[0] {
            self.compute_gate_suspect = true;
            if let Some(pdev) = self.pdev.as_ref() {
                let (installs, _, rebuilds, retains) = g17_manager::g17p_mark_counts();
                let zeros = g17_manager::G17P_MARK_ZERO_DURATION
                    .fetch_add(1, Ordering::Relaxed)
                    + 1;
                dev_err!(
                    pdev.as_ref(),
                    "G17PMARK zero-duration-retire i={} z={} r={} h={} start==end=={:#x}; the firmware retired work the GPU never executed -- core power / KSM pause gate is closed\n",
                    installs,
                    zeros,
                    rebuilds,
                    retains,
                    completion[0]
                );
            }
            // Mode 1 fails the submission honestly but does NOT rebuild.
            // MEASURED: a rebuild attempted from a gated session fails after
            // ~1.02s -- one `apple_rtkit_wait_for_completion` timeout inside
            // `apple_rtkit_boot` -- and a FAILED rebuild is destructive: it has
            // already taken `cpus`, `gfx_state` and `gfx_rtkit`, and
            // `resume_submissions` never runs, so every later ioctl is ENODEV
            // and the device is dead for the boot. A 1s stall that kills the
            // device is strictly worse than an immediate honest error, so
            // rebuilding is opt-in (mode 2) until a rebuild is known to work
            // from this state.
            if *module_parameters::g17p_zero_duration.value() != 0 {
                return Err(ETIMEDOUT);
            }
        }
        if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
            // The decisive sample: taken after a submission the GPU has
            // demonstrably executed, so a consumer cursor that is ever going to
            // move has moved by now.
            manager.log_compute_ring_state(queue, "post-completion");
            let ordinal = manager.compute_queue_submission_ordinal(queue);
            let depth = *module_parameters::g17p_trace_depth.value();
            let trace = depth == 0 || ordinal <= depth;
            if *module_parameters::g17p_completion_trace.value() != 0 && trace {
                manager.log_compute_retire(queue, completion, ordinal);
            }
            if trace {
                manager.log_compute_outer_channel("post-completion");
            }
        }
        let retire_result = self
            .manager
            .as_ref()
            .ok_or(ENODEV)?
            .complete_compute_entries(self.queue.as_mut().ok_or(ENODEV)?, 1);
        if let Err(error) = retire_result {
            return match g17_lifecycle::recover_persistent_runtime(self) {
                Ok(()) => Err(error),
                Err(recovery_error) => Err(recovery_error),
            };
        }
        if channel_path {
            self.compute_timestamps = Some(owned_timestamps);
            dev_info!(
                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                "G17P compute: submission retired from the firmware doorbell; queue entry completed\n"
            );
        } else {
            drop(self.compute_binding.take());
        }
        // Proven retired: the firmware reported this kick complete and the
        // queue entry has been advanced. A handoff may now take the cheap path.
        self.compute_abandoned = false;
        Ok(completion)
    }

    /// Log the GPU MMU fault bank.
    ///
    /// The Mesa/Vulkan dispatch fails with fault `reason = 3`, which
    /// `FAULT_REPORT_REASON` documents as the GMMU page fault, but the fault
    /// report itself carries no faulting address -- its fields stop at `+0x4c`.
    /// The address is in the MMU fault bank, and this is the only thing that
    /// tells us *which* address Mesa touched that we never mapped.
    ///
    /// This read was pulled from the session path historically because it
    /// SErrored while the GPU cores were unpowered. A submit now runs with the
    /// cores up (`gpc-state != 0`), so it is safe here in a way it was not
    /// before -- but only call it after a submit, never during startup.
    pub(crate) fn log_fault_bank(&self, when: &'static str) {
        if let Some(registers) = self.registers.as_ref() {
            let _ = registers.g17p_log_fault_bank(when);
        }
    }

    /// Drop the state a `Pending` publish parked, after a failed wait.
    /// Sample the scheduler gate (gpc-state / pause) around a submission.
    ///
    /// At depth the failure changed shape: a repeat retires cleanly and the GPU
    /// writes nothing. If the cores have powered down while the firmware still
    /// reports a completion, that shows up here and nowhere else.
    /// Compute's status page, sampled exactly as render's is -- same GPU VA,
    /// same read. DRAM only, so safe at any point including a timeout.
    pub(crate) fn log_compute_sksm_entries(&mut self, label: &'static str) {
        if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
            manager.log_compute_sksm_entries(queue, label);
        }
    }

    pub(crate) fn log_render_sksm_entries(&mut self, label: &'static str) {
        if let Some(manager) = self.manager.as_mut() {
            manager.log_render_sksm_entries(label);
        }
    }

    pub(crate) fn log_compute_status_pages(&mut self, label: &'static str) {
        if let Some(manager) = self.manager.as_mut() {
            manager.log_compute_status_pages(label);
        }
    }

    pub(crate) fn log_gate(&mut self, label: &'static str) {
        if let (Some(pdev), Some(registers)) = (self.pdev.as_ref(), self.registers.as_ref()) {
            log_g17p_scheduler_gate(pdev.as_ref(), registers, label);
        }
    }

    fn probe_render_ksm_admission_after_render_doorbells(&mut self) {
        // Called from BOTH the compute and the render submit paths, and
        // deliberately sampled before the ksm_probe gate below so one hook
        // covers both. The open question is whether the new-work interrupt
        // ever fires at all: the render window measures bit 4 clear and
        // 0x10a60 = 0, and compute is the workload that actually completes.
        if *module_parameters::g17p_irq_burst.value() >= 2 {
            if let (Some(registers), Some(bpdev)) =
                (self.registers.as_ref(), self.pdev.as_ref())
            {
                let mut seen = [(0u64, 0u64, 0u64); 12];
                match registers.g17p_burst_sample_irq(&mut seen) {
                    Ok(n) => dev_info!(
                        bpdev.as_ref(),
                        "G17P PATHIRQ distinct={} [{:#x}/{:#x}/{:#x}] [{:#x}/{:#x}/{:#x}] [{:#x}/{:#x}/{:#x}] [{:#x}/{:#x}/{:#x}]\n",
                        n,
                        seen[0].0, seen[0].1, seen[0].2,
                        seen[1].0, seen[1].1, seen[1].2,
                        seen[2].0, seen[2].1, seen[2].2,
                        seen[3].0, seen[3].1, seen[3].2,
                    ),
                    Err(error) => dev_warn!(
                        bpdev.as_ref(),
                        "G17P PATHIRQ failed ({:?})\n",
                        error,
                    ),
                }
            }
        }
        if *module_parameters::g17p_render_ksm_probe.value() == 0 {
            return;
        }
        if let Some(pdev) = self.pdev.as_ref() {
            let read = |link: u64| -> Option<u64> {
                crate::pgtable::LiveFirmwareU64Probe::new(
                    g17_manager::g17p_gfx_link_to_physical(link),
                )
                .and_then(|probe| probe.observe_mask(u64::MAX, 1))
                .map(|observation| observation.first)
                .ok()
            };
            dev_info!(
                pdev.as_ref(),
                "G17P render new-work globals: irq-mask={:#018x} latched={:#018x} pending={:#018x} events={:#018x} selector-word={:#018x}\n",
                read(0xffff_fc00_0017_9328).unwrap_or(u64::MAX),
                read(0xffff_fc00_0017_7130).unwrap_or(u64::MAX),
                read(0xffff_fc00_0017_7230).unwrap_or(u64::MAX),
                read(0xffff_fc00_0017_7200).unwrap_or(u64::MAX),
                read(0xffff_fc00_0017_85f0).unwrap_or(u64::MAX),
            );
        }
        let (Some(pdev), Some(registers)) = (self.pdev.as_ref(), self.registers.as_ref()) else {
            return;
        };
        let ta_qid = g17_resources::g17p_render_queue_id(true) as u8;
        let fragment_qid = g17_resources::g17p_render_queue_id(false) as u8;

        const POLLS: usize = 40;
        let mut last = None;
        let mut last_fault_irq = None;
        for poll in 0..POLLS {
            if poll == 2 && *module_parameters::g17p_render_3d_strobe.value() != 0 {
                // b1's state 7 releases the fragment's entry and activates its
                // descriptor BEFORE state 4 strobes; a still-parented entry
                // cannot start, which is why a bare strobe did nothing.
                let fragment_qid = g17_resources::g17p_render_queue_id(false) as u8;
                if let (Some(manager), Some(pdev)) =
                    (self.manager.as_mut(), self.pdev.as_ref())
                {
                    match manager.release_fragment_parent(1) {
                        Ok((before, after)) => dev_info!(
                            pdev.as_ref(),
                            "G17P render: DIAGNOSTIC released fragment parent link {:#x} -> {:#x}\n",
                            before,
                            after,
                        ),
                        Err(error) => dev_warn!(
                            pdev.as_ref(),
                            "G17P render: DIAGNOSTIC parent release failed ({:?})\n",
                            error,
                        ),
                    }
                }
                if let (Some(registers), Some(pdev)) =
                    (self.registers.as_ref(), self.pdev.as_ref())
                {
                    match registers.g17p_activate_queue_descriptor(fragment_qid) {
                        Ok((before, after)) => dev_info!(
                            pdev.as_ref(),
                            "G17P render: DIAGNOSTIC activated qid {} descriptor {:#018x} -> {:#018x}\n",
                            fragment_qid,
                            before,
                            after,
                        ),
                        Err(error) => dev_warn!(
                            pdev.as_ref(),
                            "G17P render: DIAGNOSTIC descriptor activate failed ({:?})\n",
                            error,
                        ),
                    }
                }
                if let (Some(registers), Some(pdev)) =
                    (self.registers.as_ref(), self.pdev.as_ref())
                {
                    let selector = *module_parameters::g17p_render_3d_strobe.value() - 1;
                    match registers.g17p_strobe_3d_launch(selector) {
                        Ok((slot, mode, strobe)) => dev_info!(
                            pdev.as_ref(),
                            "G17P render: DIAGNOSTIC 3D strobe selector={} -> slot={:#x} mode={:#018x} strobe={:#x}\n",
                            selector,
                            slot,
                            mode,
                            strobe,
                        ),
                        Err(error) => dev_warn!(
                            pdev.as_ref(),
                            "G17P render: DIAGNOSTIC 3D strobe failed ({:?})\n",
                            error,
                        ),
                    }
                }
            }
            let engine_timestamps = match self
                .manager
                .as_mut()
                .ok_or(ENODEV)
                .and_then(|manager| manager.render_engine_timestamps())
            {
                Ok(timestamps) => timestamps,
                // A compute submission has no render storage. Sample the
                // hardware anyway: this probe's whole value on the compute
                // path is the KSM slot bank and the bit-4 new-work registers,
                // which are what a working dispatch has to move.
                Err(_) => [[0, 0]; 2],
            };
            match registers.g17p_render_ksm_admission_snapshot(ta_qid, fragment_qid) {
                Ok(Some(snapshot)) => {
                    let observation = (snapshot, engine_timestamps);
                    if last != Some(observation)
                        && (snapshot.pdm_qid0_state != 0
                            || snapshot.pdm_qid0_progress != 0
                            || snapshot.ta_state != 0
                            || snapshot.ta_progress != 0
                            || snapshot.fragment_state != 0
                            || snapshot.fragment_progress != 0
                            || snapshot.qid_counters != [[0; 3]; 3]
                            || snapshot.queue_enabled != [0; 2]
                            || snapshot.current_command != 0
                            || snapshot.current_context != 0
                            || snapshot.active_queue_mask != 0
                            || snapshot.active_queue_state != 0
                            || snapshot.fault_masks != [0; 3]
                            || snapshot.dm_masks != [0; 3]
                            || snapshot.usc.is_some()
                            || engine_timestamps != [[0, 0]; 2])
                    {
                        dev_info!(
                            pdev.as_ref(),
                            "G17P render KSM admission post-doorbells poll={} gpc-state={:#x} pdm-qid0=[state={:#018x},progress={:#018x}] ta-qid{}=[state={:#018x},low6={:#x},progress={:#018x},progress-low40={:#x}] 3d-qid{}=[state={:#018x},low6={:#x},progress={:#018x},progress-low40={:#x}] current-command={:#018x} current-context={:#018x} active-queue=[{:#018x},{:#018x}] stage-masks=[{:#018x},{:#018x},{:#018x}] dm-masks=[{:#018x},{:#018x},{:#018x}] ta-ts=[{:#x},{:#x}] 3d-ts=[{:#x},{:#x}]\n",
                            poll,
                            snapshot.gpc_state,
                            snapshot.pdm_qid0_state,
                            snapshot.pdm_qid0_progress,
                            snapshot.ta_qid,
                            snapshot.ta_state,
                            snapshot.ta_state & 0x3f,
                            snapshot.ta_progress,
                            snapshot.ta_progress & ((1u64 << 40) - 1),
                            snapshot.fragment_qid,
                            snapshot.fragment_state,
                            snapshot.fragment_state & 0x3f,
                            snapshot.fragment_progress,
                            snapshot.fragment_progress & ((1u64 << 40) - 1),
                            snapshot.current_command,
                            snapshot.current_context,
                            snapshot.active_queue_mask,
                            snapshot.active_queue_state,
                            snapshot.fault_masks[0],
                            snapshot.fault_masks[1],
                            snapshot.fault_masks[2],
                            snapshot.dm_masks[0],
                            snapshot.dm_masks[1],
                            snapshot.dm_masks[2],
                            engine_timestamps[0][0],
                            engine_timestamps[0][1],
                            engine_timestamps[1][0],
                            engine_timestamps[1][1],
                        );
                        dev_info!(
                            pdev.as_ref(),
                            "G17P render KSM scheduler post-doorbells poll={} pause=[request={:#018x},status={:#018x}] stop-request=[{:#018x},{:#018x}] stopped=[{:#018x},{:#018x}] resource=[clear={:#018x}:{:#018x},go={:#018x}:{:#018x},callback={:#018x}] scheduler-status={:#018x} work-channels=[{:#018x},{:#018x},{:#018x},{:#018x}] producers16=[{:#x},{:#x},{:#x},{:#x}]\n",
                            poll,
                            snapshot.scheduling_pause_request,
                            snapshot.scheduling_pause_status,
                            snapshot.queue_stop_request[0],
                            snapshot.queue_stop_request[1],
                            snapshot.queue_stopped[0],
                            snapshot.queue_stopped[1],
                            snapshot.resource_clear[1],
                            snapshot.resource_clear[0],
                            snapshot.resource_go[1],
                            snapshot.resource_go[0],
                            snapshot.resource_callback_state,
                            snapshot.scheduler_status,
                            snapshot.work_channel_status[0],
                            snapshot.work_channel_status[1],
                            snapshot.work_channel_status[2],
                            snapshot.work_channel_status[3],
                            (snapshot.work_channel_status[0] >> 16) & 0xffff,
                            (snapshot.work_channel_status[1] >> 16) & 0xffff,
                            (snapshot.work_channel_status[2] >> 16) & 0xffff,
                            (snapshot.work_channel_status[3] >> 16) & 0xffff,
                        );
                        // Sweep the pipe window while the cores are still up:
                        // by the wait-failed checkpoint the port is no longer
                        // drivable and the read returns EAGAIN.
                        if poll == 1 && *module_parameters::g17p_pipe_sweep.value() != 0 {
                            if let Err(error) = registers.g17p_sweep_pipe_registers("poll1") {
                                dev_warn!(
                                    pdev.as_ref(),
                                    "G17P pipe sweep failed ({:?})\n",
                                    error,
                                );
                            }
                        }
                        dev_info!(
                            pdev.as_ref(),
                            "G17P render IRQ routing post-doorbells poll={} selector={:#010x} mode={:#018x} strobe={:#010x} irq=[enable={:#018x},status={:#018x},bit3={},bit4={}] completion-bitmap=[pending={:#018x},ack={:#018x}] new-work-bitmap=[pending={:#018x},ack={:#018x}]\n",
                            poll,
                            snapshot.launch_slot,
                            snapshot.launch_mode,
                            snapshot.launch_strobe,
                            snapshot.launch_irq_enable,
                            snapshot.launch_irq_status,
                            (snapshot.launch_irq_status >> 3) & 1,
                            (snapshot.launch_irq_status >> 4) & 1,
                            snapshot.launch_slot_pending,
                            snapshot.launch_slot_ack,
                            snapshot.new_work_pending,
                            snapshot.new_work_ack,
                        );
                        dev_info!(
                            pdev.as_ref(),
                            "G17P render KSM counters post-doorbells poll={} TA[outstanding={:#018x},submitted={:#018x},completed={:#018x}] 3D[outstanding={:#018x},submitted={:#018x},completed={:#018x}] enabled-mask={:#018x}:{:#018x} valid-mask={:#018x}:{:#018x} descriptor[TA]={:#018x} descriptor[3D]={:#018x} CL4[outstanding={:#018x},submitted={:#018x},completed={:#018x}] descriptor[CL4]={:#018x}\n",
                            poll,
                            snapshot.qid_counters[0][0],
                            snapshot.qid_counters[0][1],
                            snapshot.qid_counters[0][2],
                            snapshot.qid_counters[1][0],
                            snapshot.qid_counters[1][1],
                            snapshot.qid_counters[1][2],
                            snapshot.queue_enabled[1],
                            snapshot.queue_enabled[0],
                            snapshot.queue_valid_mask[1],
                            snapshot.queue_valid_mask[0],
                            snapshot.queue_descriptor[0],
                            snapshot.queue_descriptor[1],
                            snapshot.qid_counters[2][0],
                            snapshot.qid_counters[2][1],
                            snapshot.qid_counters[2][2],
                            snapshot.queue_descriptor[2],
                        );
                        dev_info!(
                            pdev.as_ref(),
                            "G17P render KSM launch poll={} dm-control={:#018x} dm-stopped={:#018x} descriptor=[TA={:#018x},3D={:#018x}] enabled=[{:#x},{:#x}] resume=[{:#x},{:#x}] pipe-vdm-stream=[{:#x},{:#x},{:#x},{:#x}] pipe-work-stamp=[{:#x},{:#x},{:#x},{:#x}] completed-by-qid=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}]\n",
                            poll,
                            snapshot.dm_control,
                            snapshot.dm_stopped,
                            snapshot.queue_descriptor[0],
                            snapshot.queue_descriptor[1],
                            snapshot.queue_enabled_mask[0],
                            snapshot.queue_enabled_mask[1],
                            snapshot.queue_resume_mask[0],
                            snapshot.queue_resume_mask[1],
                            snapshot.pipe_vdm_stream[0],
                            snapshot.pipe_vdm_stream[1],
                            snapshot.pipe_vdm_stream[2],
                            snapshot.pipe_vdm_stream[3],
                            snapshot.pipe_work_stamp[0],
                            snapshot.pipe_work_stamp[1],
                            snapshot.pipe_work_stamp[2],
                            snapshot.pipe_work_stamp[3],
                            snapshot.completed_by_qid[0],
                            snapshot.completed_by_qid[1],
                            snapshot.completed_by_qid[2],
                            snapshot.completed_by_qid[3],
                            snapshot.completed_by_qid[4],
                            snapshot.completed_by_qid[5],
                            snapshot.completed_by_qid[6],
                            snapshot.completed_by_qid[7],
                        );
                        for slot in 0..4 {
                            let progress = snapshot.slot_progress[slot];
                            dev_info!(
                                pdev.as_ref(),
                                "G17P render KSM slot {} poll={} state={:#018x} progress={:#018x} -> qid={} ts={:#x} occupied={} phase={:#x} pipe={}\n",
                                slot,
                                poll,
                                snapshot.slot_state[slot],
                                progress,
                                (progress >> 40) & 0x7f,
                                progress & 0xff_ffff_ffff,
                                snapshot.slot_state[slot] & 1,
                                (snapshot.slot_state[slot] >> 1) & 0x1f,
                                ((snapshot.slot_state[slot] >> 24) & 3) as i64 - 1,
                            );
                        }
                        if let Some(usc) = snapshot.usc {
                            dev_info!(
                                pdev.as_ref(),
                                "G17P render USC engine admission post-doorbells poll={} pipe={} serv=[vdm={:#018x},pdm={:#018x},cdm={:#018x}] debug=[vdm={:#018x},pdm={:#018x},cdm={:#018x}] rce-targets[01739,15021,14080,16068,16461,16090,16098,16429]=[{:#018x},{:#018x},{:#018x},{:#018x},{:#018x},{:#018x},{:#018x},{:#018x}]\n",
                                poll,
                                usc.pipe,
                                usc.vdm_serv,
                                usc.pdm_serv,
                                usc.cdm_serv,
                                usc.vdm_debug_status,
                                usc.pdm_debug_status,
                                usc.cdm_debug_status,
                                usc.fragment_rce_targets[0],
                                usc.fragment_rce_targets[1],
                                usc.fragment_rce_targets[2],
                                usc.fragment_rce_targets[3],
                                usc.fragment_rce_targets[4],
                                usc.fragment_rce_targets[5],
                                usc.fragment_rce_targets[6],
                                usc.fragment_rce_targets[7],
                            );
                        }
                    }
                    last = Some(observation);
                    if engine_timestamps[1].iter().all(|value| *value != 0) {
                        return;
                    }
                }
                Ok(None) => {}
                Err(error) => {
                    dev_err!(
                        pdev.as_ref(),
                        "G17P render KSM admission post-doorbells read failed ({:?})\n",
                        error,
                    );
                    return;
                }
            }
            match registers.g17p_render_fault_irq_snapshot() {
                Ok(Some(snapshot)) => {
                    if last_fault_irq != Some(snapshot)
                        && (snapshot.sub_status != 0
                            || snapshot.irq_status & (1u64 << 35) != 0
                            || snapshot.fault_info & 1 != 0)
                    {
                        dev_info!(
                            pdev.as_ref(),
                            "G17P fault IRQ post-doorbells poll={}: gpc-state={:#x} info={:#018x} addr-word={:#018x} gate={:#018x} sub-status={:#018x} irq-status={:#018x} bit35={}\n",
                            poll,
                            snapshot.gpc_state,
                            snapshot.fault_info,
                            snapshot.fault_addr_word,
                            snapshot.requestor_gate,
                            snapshot.sub_status,
                            snapshot.irq_status,
                            (snapshot.irq_status >> 35) & 1,
                        );
                    }
                    last_fault_irq = Some(snapshot);
                }
                Ok(None) => {}
                Err(error) => {
                    dev_err!(
                        pdev.as_ref(),
                        "G17P fault IRQ post-doorbells poll={} read failed ({:?})\n",
                        poll,
                        error,
                    );
                    return;
                }
            }
            if poll + 1 != POLLS {
                fsleep(Delta::from_millis(1));
            }
        }
        dev_warn!(
            pdev.as_ref(),
            "G17P render KSM admission post-doorbells ended after {}ms without a 3D timestamp\n",
            POLLS,
        );
    }

    /// Decode the completion ring; called when a doorbell wait times out.
    pub(crate) fn dump_completion_records(&mut self, label: &'static str) {
        if let Some(manager) = self.manager.as_mut() {
            let _ = manager.dump_completion_records(label);
        }
    }

    pub(crate) fn abandon_compute_submission(&mut self) {
        // The firmware was never seen to retire this kick. Remember it, so a
        // later client handoff takes the CPU-stopping path rather than the
        // cheap one.
        self.compute_abandoned = true;
        drop(self.pending_compute.take());
        // Ring/channel state at the moment of failure. This is the sample that
        // was missing: the detailed timeout dump that already existed only ever
        // ran on the legacy in-submit poll path (`g17p_irq_completion=0`), and
        // the condvar timeout we actually hit printed nothing about the queue.
        //
        // DRAM only, deliberately, and this is the exact spot where that
        // matters: by the time a submit has timed out the GPU cores have
        // powered back down, so anything touching the sgx register file here
        // takes an asynchronous SError and panics instead of reporting. Every
        // field below is a plain read of an object the driver itself allocated.
        if let (Some(manager), Some(queue)) = (self.manager.as_mut(), self.queue.as_mut()) {
            manager.log_compute_ring_state(queue, "timeout");
        }
        if let Some(manager) = self.manager.as_mut() {
            manager.log_compute_outer_channel("timeout");
            // Scan BOTH KSM completion buffers, ordinal 0 and ordinal 2.
            // `read_compute_completion` only ever looks at ordinal 0, and only
            // ordinal 0 is cleared at publish -- so if the firmware routes some
            // completions to ordinal 2 we would see exactly this: a submission
            // that ran fine and a wait that never ends. This scan reports every
            // non-zero word in both, which settles it. Also DRAM only.
            // Decoded first: one line per populated completion record, plus
            // whether the descriptor this submission is waiting for is present
            // anywhere in the ring. That single fact splits "the firmware
            // reported nothing" from "it reported and the reader rejected it".
            let _ = manager.dump_compute_completion_records("timeout");
            // Raw words as a backstop; it truncates at 24 non-zero words, which
            // is about three records, so it is no longer the primary view.
            let _ = manager.dump_completion_stamps("timeout");
        }
    }

    /// Build and retain the exact T8140 dual-role runtime.
    pub(crate) fn start(
        pdev: &platform::Device<Core>,
        drm: ARef<AsahiDevice>,
        registers: regs::Resources,
        soc: &'static hw::agx3::SocConfig,
        id: &identity::GpuIdentity,
        observed: &hw::GpuIdConfig,
        _topology: T8140LiveTopologyAdmission,
    ) -> Result<Self> {
        if soc.chip_id != 0x8140
            || id.gpu_gen != hw::GpuGen::G17
            || id.gpu_variant != identity::GpuVariant::P
        {
            return Err(EINVAL);
        }
        let test_stage =
            G17PTestStage::from_parameter(*module_parameters::g17p_test_stage.value())?;
        // `observed` was populated by the sole safe SGX GPU-ID probe before
        // either ASC starts. Retain its hardware-derived values rather than
        // re-reading SGX here after ownership has begun moving into runtime.
        let num_clusters = observed.num_clusters;

        dev_info!(
            pdev.as_ref(),
            "G17P live-boot: {:?} admitted before effects; selected {} stage\n",
            BootStage::ExactTopology,
            test_stage.name()
        );
        dev_info!(
            pdev.as_ref(),
            "G17P diagnostic: scheduler-root-checkpoints-v1 armed\n"
        );

        let pdev_ref: ARef<platform::Device> = pdev.into();
        let (manager, session) = match boot_initial_session(
            &pdev_ref,
            &drm,
            &registers,
            soc,
            num_clusters,
            observed.gpc_perf_state_map,
            observed.gpc_perf_state_map_low,
            observed.gpc_perf_state_control,
            test_stage,
        ) {
            Ok(runtime) => runtime,
            Err(error) => {
                dev_err!(
                    pdev.as_ref(),
                    "G17P live-boot: initial session failed ({:?})\n",
                    error
                );
                return Err(error);
            }
        };
        // Render admission is independent of whether compute has ever been
        // used. The first compute submit ensures its own graph synchronously.
        let accepting_submissions = session.compute_uapi_enabled;

        Ok(Self {
            pdev: Some(pdev_ref),
            drm: Some(drm),
            registers: Some(registers),
            manager: Some(manager),
            cpus: Some(session.cpus),
            gfx_state: Some(session.gfx_state),
            gfx_rtkit: Some(session.gfx_rtkit),
            gfx1_state: Some(session.gfx1_state),
            gfx1_rtkit: Some(session.gfx1_rtkit),
            queue: session.queue,
            compute_binding: None,
            compute_timestamps: None,
            completion_indices: g17_completion::QueueIndices {
                done: 0,
                read: 0,
                write: 0,
            },
            render_tracker: None,
            test_stage,
            accepting_submissions,
            queue_setup_pending: session.queue_setup_pending,
            compute_owner: None,
            compute_recycle_pending: false,
            compute_abandoned: false,
            retired_queues: KVec::new(),
            async_compute_queues: KVec::new(),
            async_visibility_pending: false,
            quarantined_bindings: KVec::new(),
            crash_recoveries: 0,
            compute_gate_suspect: false,
            completion_signal: G17PCompletionSignal::new()?,
            pending_compute: None,
        })
    }

    /// Publish first graphics in the measured 3D/TA order and send its one
    /// pair-zero/channel-0 work notification through the retained primary
    /// RTKit endpoint.
    pub(crate) fn submit_partial_opening_render<F>(
        &mut self,
        submission: g17_manager::G17PPartialOpeningRenderSubmission,
        publish_late_tiling_state: F,
    ) -> Result
    where
        F: FnOnce(),
    {
        if self.render_tracker.is_some() {
            return Err(EBUSY);
        }
        let manager = self.manager.as_mut().ok_or(ENODEV)?;
        let gfx_rtkit = self.gfx_rtkit.as_mut().ok_or(ENODEV)?;
        let (tracker, prepared) =
            manager.submit_partial_opening_render(submission, publish_late_tiling_state)?;
        Pin::new(gfx_rtkit)
            .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, prepared.paired_doorbell)?;
        self.render_tracker = Some(tracker);
        Ok(())
    }

    /// Consume queue progress together with the scheduler job-list state and
    /// the owned start/end timestamps required for execution completion.
    pub(crate) fn observe_partial_opening_render_completion(
        &mut self,
        tiling_current: g17_completion::QueueIndices,
        fragment_current: g17_completion::QueueIndices,
        execution: g17_completion::RenderExecutionState,
    ) -> Result<g17_completion::PairedRenderCompletion> {
        let manager = self.manager.as_ref().ok_or(ENODEV)?;
        let tracker = self.render_tracker.as_mut().ok_or(ENODEV)?;
        let completion = manager.observe_partial_opening_render_completion(
            tracker,
            tiling_current,
            fragment_current,
            execution,
        )?;
        if completion.complete {
            self.render_tracker = None;
        }
        Ok(completion)
    }

    /// Apply one fully routed normal-CL completion to the retained queue.
    ///
    /// The physical IRQ, ordinal-0 descriptor, MMIO acknowledgement, and
    /// EP0x20 stamp-event route remain closed. This legacy boundary therefore
    /// rejects the incomplete selector-2-only input before queue mutation.
    pub(crate) fn observe_compute_completion(
        &mut self,
        raw: &[u8],
        current: g17_completion::QueueIndices,
        published_prefix: u32,
    ) -> Result<g17_completion::QueuePrefixState> {
        let _ = (raw, current, published_prefix);
        if g17_completion::require_g17p_normal_cl_completion_route(
            g17_completion::G17P_NORMAL_CL_COMPLETION_ROUTE,
        )
        .is_err()
        {
            dev_err!(
                self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                "G17P completion: live ordinal0 IRQ/EP0x20 route unavailable; outstanding unchanged\n"
            );
            return Err(ENOTSUPP);
        }
        dev_err!(
            self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P completion: legacy selector2 input lacks ordinal0 stamp correlation; outstanding unchanged\n"
        );
        Err(ENOTSUPP)
    }

    /// Commit only a transaction which already passed ordinal-0 entry and
    /// EP0x20 stamp correlation. The route gate remains closed until Linux
    /// owns the physical IRQ, firmware-event ring, and completion ACK writer.
    pub(crate) fn apply_prepared_normal_cl_completion(
        &mut self,
        prepared: g17_completion::PreparedG17PNormalClCompletion,
    ) -> Result {
        g17_completion::require_g17p_normal_cl_completion_route(
            g17_completion::G17P_NORMAL_CL_COMPLETION_ROUTE,
        )
        .map_err(|_| ENOTSUPP)?;
        let manager = self.manager.as_ref().ok_or(ENODEV)?;
        let queue = self.queue.as_mut().ok_or(ENODEV)?;
        if prepared.entry.queue_id != manager.compute_queue_id(queue)? {
            return Err(EINVAL);
        }
        manager.complete_compute_entries(queue, prepared.completed_generation)
    }
}

impl g17_lifecycle::PersistentRuntimeRecovery for G17PLiveRuntime {
    type Error = Error;

    fn pause_submissions(&mut self) {
        self.accepting_submissions = false;
    }

    fn stop_cpu_pair(&mut self) -> Result {
        match self.cpus.as_mut() {
            Some(cpus) => cpus.stop(),
            None => Ok(()),
        }
    }

    fn drop_gfx1_rtkit(&mut self) {
        drop(self.gfx1_rtkit.take());
        drop(self.gfx1_state.take());
    }

    fn drop_gfx_rtkit(&mut self) {
        drop(self.gfx_rtkit.take());
        drop(self.gfx_state.take());
    }

    fn disable_sksm_queue(&mut self) -> Result {
        if let Some(queue) = self.queue.as_mut() {
            self.manager
                .as_ref()
                .ok_or(ENODEV)?
                .unregister_compute_sksm_queue(self.registers.as_ref().ok_or(ENODEV)?, queue)?;
        }
        Ok(())
    }

    fn release_runtime_objects(&mut self) {
        drop(self.pending_compute.take());
        drop(self.compute_timestamps.take());
        drop(self.compute_binding.take());
        drop(self.queue.take());
        // Both CPUs have stopped by the time this runs, so the graphs a
        // client handoff retired can finally be freed: nothing in the
        // firmware can still dereference them.
        self.retired_queues.clear();
        self.async_compute_queues.clear();
        self.async_visibility_pending = false;
        // Both CPUs have stopped, so a quarantined client's page tables and
        // GEM objects can finally go too.
        self.quarantined_bindings.clear();
        self.compute_owner = None;
        self.compute_recycle_pending = false;
        self.compute_abandoned = false;
        self.compute_gate_suspect = false;
        if let Some(manager) = self.manager.as_mut() {
            manager.release_runtime_objects_after_processor_stop();
        }
        self.render_tracker = None;
        self.completion_indices = g17_completion::QueueIndices {
            done: 0,
            read: 0,
            write: 0,
        };
    }

    fn rebuild_session_in_place(&mut self) -> Result {
        drop(self.cpus.take());
        let pdev = self.pdev.as_ref().ok_or(ENODEV)?.clone();
        let drm = self.drm.as_ref().ok_or(ENODEV)?.clone();
        let registers = self.registers.as_ref().ok_or(ENODEV)?;
        let manager = self.manager.as_mut().ok_or(ENODEV)?;
        manager.reset_session_after_processor_stop()?;
        let session = boot_session(&pdev, &drm, registers, manager, self.test_stage)?;
        self.cpus = Some(session.cpus);
        self.gfx_state = Some(session.gfx_state);
        self.gfx_rtkit = Some(session.gfx_rtkit);
        self.gfx1_state = Some(session.gfx1_state);
        self.gfx1_rtkit = Some(session.gfx1_rtkit);
        self.queue = session.queue;
        self.queue_setup_pending = session.queue_setup_pending;
        self.arm_primary_firmware_event_worker()?;
        Ok(())
    }

    fn resume_submissions(&mut self) {
        // As on initial boot, admission must not depend on the lazy compute
        // graph. Render can be first after recovery; compute ensures its own
        // graph synchronously when submitted.
        self.accepting_submissions = self.test_stage.enables_compute_uapi();
    }
}

impl g17_lifecycle::PersistentRuntimeTeardown for G17PLiveRuntime {
    fn stop_cpu_pair(&mut self) -> bool {
        let result = match self.cpus.as_mut() {
            Some(cpus) => cpus.stop(),
            None => Ok(()),
        };
        match result {
            Ok(()) => true,
            Err(error) => {
                if let Some(pdev) = self.pdev.as_ref() {
                    dev_err!(
                        pdev.as_ref(),
                        "G17P teardown: CPU stop failed ({:?}); retaining firmware-owned state\n",
                        error
                    );
                } else {
                    pr_err!(
                        "G17P teardown: CPU stop failed ({:?}); retaining firmware-owned state\n",
                        error
                    );
                }
                false
            }
        }
    }

    fn retain_after_stop_failure(&mut self) {
        self.accepting_submissions = false;
        core::mem::forget(self.compute_timestamps.take());
        core::mem::forget(self.compute_binding.take());
        core::mem::forget(self.queue.take());
        core::mem::forget(core::mem::take(&mut self.retired_queues));
        core::mem::forget(core::mem::take(&mut self.quarantined_bindings));
        core::mem::forget(self.gfx1_rtkit.take());
        core::mem::forget(self.gfx1_state.take());
        core::mem::forget(self.gfx_rtkit.take());
        core::mem::forget(self.gfx_state.take());
        core::mem::forget(self.cpus.take());
        core::mem::forget(self.manager.take());
        core::mem::forget(self.drm.take());
        core::mem::forget(self.registers.take());
        core::mem::forget(self.pdev.take());
    }

    fn unregister_queue(&mut self) {
        if let (Some(manager), Some(registers), Some(queue)) = (
            self.manager.as_ref(),
            self.registers.as_ref(),
            self.queue.as_mut(),
        ) {
            let _ = manager.unregister_compute_sksm_queue(registers, queue);
        }
    }

    fn drop_queue(&mut self) {
        drop(self.compute_timestamps.take());
        drop(self.compute_binding.take());
        drop(self.queue.take());
        self.retired_queues.clear();
        self.async_compute_queues.clear();
        self.async_visibility_pending = false;
        self.quarantined_bindings.clear();
        self.compute_owner = None;
        self.compute_recycle_pending = false;
        self.compute_abandoned = false;
    }

    fn drop_gfx1_rtkit(&mut self) {
        drop(self.gfx1_rtkit.take());
        drop(self.gfx1_state.take());
    }

    fn drop_gfx_rtkit(&mut self) {
        drop(self.gfx_rtkit.take());
        drop(self.gfx_state.take());
    }

    fn drop_cpu_pair(&mut self) {
        drop(self.cpus.take());
    }

    fn drop_manager(&mut self) {
        drop(self.manager.take());
    }
}

impl Drop for G17PLiveRuntime {
    fn drop(&mut self) {
        g17_lifecycle::teardown_persistent_runtime(self);
        drop(self.drm.take());
        drop(self.registers.take());
        drop(self.pdev.take());
    }
}

include!("g17_async_compute.rs");
