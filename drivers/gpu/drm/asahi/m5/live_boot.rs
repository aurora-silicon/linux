// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5-specific initialization and retained application submission session.
//! DRM admission remains in the device adapter; no G17P manager is selected.

use super::provider::{validate_provider, ExpectedProvider, G17PAscwrapV6Role,
    G17PMailboxProvider, G17PNoRtkitBuffer};
use crate::{driver::AsahiDrmDevice, m5::resources::M5FirmwareMemory,
    m5::fw_msg, mmu, regs};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use crate::m5::completion::{graphics_pointer_head, MAX_GRAPHICS_GENERATION};

/// Application jobs are polled for retirement. A 1 ms step put up to a
/// millisecond of idle GPU after every job, several per video frame.
/// (The first render job keeps 1 ms steps; see submit_application.)
const APPLICATION_POLL_US: i64 = 50;
const APPLICATION_POLLS_PER_MS: u32 = 1000 / APPLICATION_POLL_US as u32;
use kernel::{device::Core, macros::vtable, platform, prelude::*,
    soc::apple::rtkit, sync::{aref::ARef, Arc}, time::{delay::fsleep, Delta}};

struct M5RoleState {
    dev: ARef<AsahiDrmDevice>,
    role: usize,
    acked: AtomicBool,
    crashed: AtomicBool,
    events: AtomicU32,
}

struct M5RtkitOps;

#[vtable]
impl rtkit::Operations for M5RtkitOps {
    type Data = Arc<M5RoleState>;
    type Buffer = G17PNoRtkitBuffer;

    fn recv_message_early(
        data: <Self::Data as kernel::types::ForeignOwnable>::Borrowed<'_>,
        endpoint: u8,
        message: u64,
    ) -> bool {
        // Both native M5 roles reply with exactly 0x0009000000000000.
        if endpoint == 0x20 && message == 0x0009_0000_0000_0000 {
            data.acked.store(true, Ordering::Release);
            true
        } else {
            false
        }
    }

    fn recv_message(
        data: <Self::Data as kernel::types::ForeignOwnable>::Borrowed<'_>,
        endpoint: u8,
        message: u64,
    ) {
        let ordinal = data.events.fetch_add(1, Ordering::Relaxed);
        if ordinal < 8 {
            dev_dbg!(data.dev.as_ref(), "M5 message role={} ep={:#x} payload={:#x}\n",
                data.role, endpoint, message);
        }
    }

    fn crashed(
        data: <Self::Data as kernel::types::ForeignOwnable>::Borrowed<'_>,
        _crashlog: Option<&[u8]>,
    ) {
        data.crashed.store(true, Ordering::Release);
        dev_err!(data.dev.as_ref(), "M5 firmware crashed role={}\n", data.role);
    }
}

struct M5Role {
    provider: G17PMailboxProvider,
    rtkit: rtkit::RtKit<M5RtkitOps>,
    state: Arc<M5RoleState>,
    started: bool,
}

struct M5Session {
    roles: [M5Role; 2],
    memory: Option<KBox<M5FirmwareMemory>>,
    uat: Arc<mmu::Uat>,
    /// The GPU mappings of both firmware crash log buffers.
    _crash_views: [mmu::KernelMapping; 2],
    registers: regs::G17Resources,
    dev: ARef<AsahiDrmDevice>,
}

impl M5Session {
    fn stop_cpus(&mut self) -> Result {
        let mut failure = None;
        // Reverse the measured M5 GFX1/GFX start order.
        for index in [0, 1] {
            if self.roles[index].started {
                match self.roles[index].provider.stop_cpu() {
                    Ok(()) => self.roles[index].started = false,
                    Err(error) => failure = Some(error),
                }
            }
        }
        failure.map_or(Ok(()), Err)
    }

    fn initialize(&mut self) -> Result {
        self.registers.m5_match_pmgr_boot_control()?;
        for index in [1, 0] {
            let role = &mut self.roles[index];
            role.provider.start_cpu()?;
            role.started = true;
            Pin::new(&mut role.rtkit).boot()?;
            dev_dbg!(self.dev.as_ref(), "M5_INIT role={} rtkit-ready\n", index);
        }
        // These helpers operate only on the proven shared dual-root layout;
        // the Neo-specific contexts, metrics and manager remain unselected.
        let roots = self.uat.adopt_firmware_root_entries()?;
        let timestamp: u64;
        // SAFETY: Read-only architectural counter, available on this target.
        unsafe { core::arch::asm!("mrs {value}, cntvct_el0", value = out(reg) timestamp,
            options(nomem, nostack, preserves_flags)); }
        self.memory = Some(M5FirmwareMemory::new_with_uat(
            &self.dev, self.uat.clone(), timestamp)?);
        self.uat.confirm_firmware_root_entries(roots)?;
        self.uat.mirror_secondary_root()?;
        dev_dbg!(self.dev.as_ref(), "M5_INIT retained memory graph mapped\n");
        self.registers.enable_g17_gating()?;

        // M5's own trace publishes and ACKs GFX1 first (133/137), then GFX
        // (160/164). The Neo's opposite ordering is not used here.
        for index in [1, 0] {
            let address = self.memory.as_ref().ok_or(EIO)?.root(index == 1);
            let message = fw_msg::encode_initdata_doorbell(address).map_err(|_| EINVAL)?;
            let role = &mut self.roles[index];
            Pin::new(&mut role.rtkit).start_endpoint(0x20)?;
            Pin::new(&mut role.rtkit).start_endpoint(0x21)?;
            Pin::new(&mut role.rtkit).send_message(0x20, message)?;
            dev_dbg!(self.dev.as_ref(), "M5_INIT role={} published root={:#x}\n", index, address);
            for _ in 0..200 {
                if self.memory.as_mut().ok_or(EIO)?.drain_reports(&self.dev)? {
                    return Err(EIO);
                }
                if role.state.crashed.load(Ordering::Acquire) {
                    return Err(EIO);
                }
                if role.state.acked.load(Ordering::Acquire) {
                    break;
                }
                fsleep(Delta::from_millis(10));
            }
            if !role.state.acked.load(Ordering::Acquire) {
                return Err(ETIMEDOUT);
            }
            dev_dbg!(self.dev.as_ref(), "M5_INIT role={} initdata-ack\n", index);
        }
        if self.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)) {
            return Err(EIO);
        }
        self.retire_openings()
    }

    fn install_graphics_queues(&mut self) -> Result {
        let memory = self.memory.as_mut().ok_or(EIO)?;
        dev_dbg!(self.dev.as_ref(), "M5_GRAPHICS install before counters={:x?} registration={:x?}\n",
            memory.graphics_status()?, memory.graphics_installation_status()?);
        memory.publish_graphics_install()?;
        Pin::new(&mut self.roles[0].rtkit).send_message(0x21, 0x0083_0000_0000_000a)?;
        for poll in 0..1000 {
            if memory.drain_reports(&self.dev)? { return Err(EIO); }
            if self.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)) {
                return Err(EIO);
            }
            let (outer, inner, _) = memory.graphics_status()?;
            if outer == [2; 3] && inner == [[1; 3]; 2]
                && memory.graphics_queues_installed()? {
                dev_dbg!(self.dev.as_ref(), "M5_GRAPHICS installed TA/fragment queues; registration={:x?}; no draws published\n",
                    memory.graphics_installation_status()?);
                return Ok(());
            }
            if [0, 100].contains(&poll) {
                dev_dbg!(self.dev.as_ref(), "M5_GRAPHICS install poll={} counters={:x?} registration={:x?}\n",
                    poll, memory.graphics_status()?, memory.graphics_installation_status()?);
            }
            fsleep(Delta::from_millis(1));
        }
        dev_err!(self.dev.as_ref(), "M5_GRAPHICS install timeout counters={:x?} registration={:x?}\n",
            memory.graphics_status()?, memory.graphics_installation_status()?);
        Err(ETIMEDOUT)
    }

    fn retire_idle_policy(&mut self) -> Result {
        let memory = self.memory.as_mut().ok_or(EIO)?;
        // Retire the idle-policy request.
        memory.publish_idle_wake()?;
        Pin::new(&mut self.roles[0].rtkit).send_message(0x21, 0x0084_0000_0000_0011)?;
        for _ in 0..1000 {
            if memory.drain_reports(&self.dev)? { return Err(EIO); }
            if self.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)) {
                dev_err!(self.dev.as_ref(), "M5: firmware crashed retiring the idle policy\n");
                return Err(EIO);
            }
            if memory.control_counters()? == [[2; 3], [1; 3]] {
                return Ok(());
            }
            fsleep(Delta::from_millis(1));
        }
        dev_err!(self.dev.as_ref(), "M5: idle policy not retired, counters={:?}\n",
            memory.control_counters()?);
        Err(ETIMEDOUT)
    }

    fn retire_openings(&mut self) -> Result {
        let mut counters = self.memory.as_mut().ok_or(EIO)?.control_counters()?;
        dev_dbg!(self.dev.as_ref(), "M5_OPEN before power-transition-end {:?}\n", counters);
        // M5's own cold trace sends one 0x89 to GFX, then GFX1 (events
        // 167/171), immediately after the two initdata ACKs. No other control
        // record or work item is published in this bounded experiment.
        for index in [0, 1] {
            Pin::new(&mut self.roles[index].rtkit)
                .send_message(0x21, fw_msg::encode_power_transition_end())?;
        }
        for _ in 0..1000 {
            if self.memory.as_mut().ok_or(EIO)?.drain_reports(&self.dev)? {
                return Err(EIO);
            }
            if self.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)) {
                return Err(EIO);
            }
            counters = self.memory.as_mut().ok_or(EIO)?.control_counters()?;
            if counters == [[1; 3]; 2] {
                dev_dbg!(self.dev.as_ref(), "M5_OPEN both native control openings retired {:?}\n", counters);
                return Ok(());
            }
            fsleep(Delta::from_millis(1));
        }
        dev_err!(self.dev.as_ref(), "M5_OPEN control openings did not retire {:?}\n", counters);
        Err(ETIMEDOUT)
    }

}

/// Sizes of the GFX and GFX1 `__DATA` segments the 25G76 firmware runs with.
const DATA_SIZES: [u64; 2] = [0x134000, 0x130000];
/// The firmware's crash log buffer, at this offset of each `__DATA` segment.
const CRASH_LOG_OFFSET: u64 = 0xa0000;
const CRASH_LOG_SIZE: usize = 0x8000;

/// Creates the UAT both firmware instances share, as the A18 Pro's: each instance publishes
/// its own kernel root, the secondary one 0x40000 into `pagetables`. Each instance asks for
/// its crash log at the GPU address of `__DATA` + 0xa0000 truncated to 40 bits, which is
/// mapped here, before either instance starts.
fn new_uat(dev: &AsahiDrmDevice) -> Result<(Arc<mmu::Uat>, [mmu::KernelMapping; 2])> {
    let node = dev.as_ref().as_ref().of_node().ok_or(EINVAL)?;
    let mut crash_logs = [0u64; 2];
    for (index, name) in [c"gfx-data", c"gfx1-data"].into_iter().enumerate() {
        let region = node.reserved_mem_region_to_resource_byname(name)?;
        if region.size() != DATA_SIZES[index] || region.start() & (mmu::UAT_PGSZ as u64 - 1) != 0
        {
            return Err(EINVAL);
        }
        crash_logs[index] = region.start() + CRASH_LOG_OFFSET;
    }
    let uat = Arc::new(
        mmu::Uat::new(
            dev.into(),
            mmu::UatGeometry { ias: 42, oas: 42 },
            false,
            mmu::UatFirmware::PublishedRoots { secondary_root_offset: Some(0x40000) },
            mmu::UatQuirks::DEFAULT,
        )?,
        GFP_KERNEL,
    )?;
    let map = |pa: u64| {
        uat.kernel_lower_vm().map_io(
            dev.as_ref().as_ref(),
            pa & ((1u64 << 40) - 1),
            pa.try_into()?,
            CRASH_LOG_SIZE,
            mmu::PROT_FW_SHARED_RW,
        )
    };
    let views = [map(crash_logs[0])?, map(crash_logs[1])?];
    Ok((uat, views))
}

/// Set once the module is pinned for an M5 session, which is retained until reboot.
static M5_MODULE_RETAINED: AtomicBool = AtomicBool::new(false);

pub(crate) struct M5Runtime {
    session: Option<KBox<M5Session>>,
    application: bool,
    faulted: bool,
    application_generation: u32,
    application_compute_generation: u32,
}

impl M5Runtime {
    fn prepare(
        pdev: &platform::Device<Core<'_>>,
        dev: ARef<AsahiDrmDevice>,
        registers: regs::G17Resources,
    ) -> Result<Self> {
        let gfx = G17PMailboxProvider::get(pdev.as_ref(), 0)?;
        let gfx1 = G17PMailboxProvider::get(pdev.as_ref(), 1)?;
        for (provider, expected) in [
            (&gfx, ExpectedProvider { role: G17PAscwrapV6Role::Gfx,
                wrapper: (0x582600000, 0x88000), iop_vbar: (0x582050000, 8) }),
            (&gfx1, ExpectedProvider { role: G17PAscwrapV6Role::Gfx1,
                wrapper: (0x582e00000, 0x88000), iop_vbar: (0x582850000, 8) }),
        ] {
            validate_provider(provider, expected)?;
            provider.require_safe_cpu_lifecycle()?;
        }
        let (uat, crash_views) = new_uat(&dev)?;
        let make_role = |index, provider| -> Result<M5Role> {
            let state = Arc::new(M5RoleState { dev: dev.clone(), role: index,
                acked: AtomicBool::new(false), crashed: AtomicBool::new(false),
                events: AtomicU32::new(0) }, GFP_KERNEL)?;
            let rtkit = rtkit::RtKit::<M5RtkitOps>::new_with_firmware_crashlog(
                dev.as_ref().as_ref(), None, index, state.clone())?;
            Ok(M5Role { provider, rtkit, state, started: false })
        };
        let session = KBox::new(M5Session {
            roles: [make_role(0, gfx)?, make_role(1, gfx1)?],
            memory: None, uat, _crash_views: crash_views, registers, dev,
        }, GFP_KERNEL)?;
        // From this point every path returns the retained platform owner,
        // even when firmware rejects the graph. Failed probe unwinding must
        // never release UAT or transports beneath a running firmware CPU.
        // Keep callback code resident even if a failed stop retains owners.
        // This explicit RAM-boot experiment is released by reboot only. No
        // CPU has started yet: once module removal has begun, refuse with
        // nothing started and the session freed.
        crate::retain::retain_module(&M5_MODULE_RETAINED)?;
        Ok(Self { session: Some(session), application: false, faulted: false,
            application_generation: 0, application_compute_generation: 0 })
    }

    /// Boot without a native program or job graph and leave both processors
    /// running for explicit application submissions. This does not register
    /// a render node or claim that Mesa command translation is complete.
    pub(crate) fn start_application(
        pdev: &platform::Device<Core<'_>>, dev: ARef<AsahiDrmDevice>, registers: regs::G17Resources,
    ) -> Result<Self> {
        // Periodic power/submission traces need continuous draining while
        // the session idles. Recovery bit 9 emits only on a fault, which the
        // synchronous submit loop drains before retaining a failed graph.
        if crate::module_parameters::m5_fw_trace.value() & !0x200 != 0 {
            return Err(EINVAL);
        }
        let mut owner = Self::prepare(pdev, dev, registers)?;
        owner.application = true;
        let result = (|| {
            let session = owner.session.as_mut().ok_or(EIO)?;
            session.initialize()?;
            session.retire_idle_policy()?;
            Ok(())
        })();
        if let Err(error) = result {
            owner.faulted = true;
            return Err(error);
        }
        dev_info!(pdev.as_ref(), "M5: GPU firmware 25G76 running\n");
        Ok(owner)
    }

    pub(crate) fn new_user_vm(&self, id: u64, kernel_range: core::ops::Range<u64>) -> Result<mmu::Vm> {
        if !self.application || self.is_crashed() { return Err(ENODEV); }
        crate::m5::vm_layout::compact_range(&kernel_range).ok_or(EINVAL)?;
        self.session.as_ref().ok_or(EIO)?.uat.new_vm(id, kernel_range)?.with_status()
    }

    pub(crate) fn is_crashed(&self) -> bool {
        self.faulted || self.session.as_ref().is_none_or(|session|
            session.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)))
    }

    /// The adapter serializes this call and validates the translated command
    /// plus every user mapping before entry. Return success only after paired
    /// queue retirement, scheduler unlink, stamps and firmware notifications.
    /// A failed published job retains and quarantines its VM until reboot.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn submit_application(
        &mut self, vm: &mmu::Vm, parameters: crate::m5::render_desc::G17pRenderParameters,
        controls: crate::m5::render::M5RenderControls, clusters: u32,
        usc_required: [u64; 2],
    ) -> Result<crate::m5::completion::M5GraphicsCompletion> {
        if !self.application || self.is_crashed() { return Err(ENODEV); }
        // Reuse retired ring storage while keeping completion stamps unique.
        if self.application_generation >= MAX_GRAPHICS_GENERATION { return Err(EBUSY); }
        let generation = self.application_generation + 1;
        let session = self.session.as_deref_mut().ok_or(EIO)?;
        if generation == 1 {
            session.memory.as_mut().ok_or(EIO)?.initialize_application_graphics(
                &session.dev, vm, parameters, controls, clusters, usc_required)?;
        }
        // From installation onward, even a failed mailbox send can have made
        // memory visible to firmware. Never unwind or recycle the job graph.
        self.application_generation = generation;
        let result = (|| {
            if generation == 1 { session.install_graphics_queues()?; }
            let memory = session.memory.as_mut().ok_or(EIO)?;
            if generation == 1 {
                memory.publish_graphics_job()?;
            } else {
                memory.publish_next_application_job(&session.dev, vm, parameters, controls,
                    clusters, usc_required, generation)?;
            }
            Pin::new(&mut session.roles[0].rtkit).send_message(0x21, 0x0083_0000_0000_000a)?;
            for _ in 0..10000 * APPLICATION_POLLS_PER_MS {
                if memory.drain_reports(&session.dev)?
                    || session.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)) {
                    return Err(EIO);
                }
                let (outer, inner, _) = memory.graphics_status()?;
                let completion = memory.graphics_completion()?;
                if outer == [memory.application_outer_head(); 3]
                    && inner == [[graphics_pointer_head(generation); 3]; 2]
                    && M5FirmwareMemory::graphics_pair_complete(completion, generation) {
                    dev_dbg!(session.dev.as_ref(),
                        "M5_APPLICATION generation={} completed; both queues retired\n",
                        generation);
                    return Ok(completion);
                }
                // The first job also installs the queues; polling it this
                // often met a firmware recovery on its fragment queue.
                fsleep(Delta::from_micros(if generation == 1 { 1000 } else { APPLICATION_POLL_US }));
            }
            Err(ETIMEDOUT)
        })();
        if let Err(error) = result {
            if let Some(memory) = session.memory.as_mut() {
                for _ in 0..4 { let _ = memory.drain_reports(&session.dev); }
                dev_err!(session.dev.as_ref(),
                    "M5_APPLICATION failed graph counters={:x?} completion={:x?}\n",
                    memory.graphics_status(), memory.graphics_completion());
            }
            self.faulted = true;
            if let Some(status) = vm.status() { status.record(error); }
            if let Some(memory) = &session.memory { memory.quarantine_graphics(); }
            // A successful stop request alone does not prove a halt. Keep
            // the full session even after this best-effort stop succeeds.
            let stopped = session.stop_cpus();
            dev_err!(session.dev.as_ref(),
                "M5_APPLICATION submission failed {:?}; stop={:?}; VM and firmware owners retained\n",
                error, stopped);
        }
        result
    }

    /// Execute a compute command in the already retained application VM.
    /// Installation, dispatch and retirement are separate firmware transitions.
    /// The first submission installs queue 12 and dispatches generation 1;
    /// every later one reuses that owner and dispatches the next generation,
    /// as submit_application() does for the graphics pair.
    pub(crate) fn submit_application_compute(&mut self, vm: &mmu::Vm,
        command: &crate::m5::command::TranslatedComputeCommand) -> Result {
        if !self.application || self.is_crashed() || self.application_generation == 0 {
            return Err(ENOTSUPP);
        }
        if self.application_compute_generation >= MAX_GRAPHICS_GENERATION {
            return Err(EBUSY);
        }
        let generation = self.application_compute_generation + 1;
        let session = self.session.as_deref_mut().ok_or(EIO)?;
        if generation == 1 {
            session.memory.as_mut().ok_or(EIO)?.initialize_application_compute(
                &session.dev, vm, command, self.application_generation)?;
        } else {
            session.memory.as_mut().ok_or(EIO)?.prepare_next_application_compute(
                &session.dev, vm, command, generation)?;
        }
        // The owner is retained from installation onward. Even a failed mailbox
        // send can follow firmware seeing the new producer, so never unwind.
        self.application_compute_generation = generation;
        let result = (|| {
            let memory = session.memory.as_mut().ok_or(EIO)?;
            // Installation is published once; the dispatch record every time.
            let stages: &[bool] = if generation == 1 { &[false, true] } else { &[true] };
            for &dispatch in stages {
                memory.publish_application_compute(dispatch, generation)?;
                Pin::new(&mut session.roles[0].rtkit)
                    .send_message(0x21, 0x0083_0000_0000_000a)?;
                let mut retired = false;
                for _ in 0..if dispatch { 10000 } else { 1000 } * APPLICATION_POLLS_PER_MS {
                    if memory.drain_reports(&session.dev)?
                        || session.roles.iter().any(|role| role.state.crashed.load(Ordering::Acquire)) {
                        return Err(EIO);
                    }
                    let (outer, _, _) = memory.graphics_status()?;
                    if outer == [memory.application_outer_head(); 3]
                        && if dispatch { memory.application_compute_complete(generation)? }
                            else { memory.application_compute_installed()? } {
                        retired = true;
                        break;
                    }
                    fsleep(Delta::from_micros(APPLICATION_POLL_US));
                }
                if !retired { return Err(ETIMEDOUT); }
                dev_dbg!(session.dev.as_ref(),
                    "M5_APPLICATION_COMPUTE {} generation={} queue=12 outer={} status={:x?}\n",
                    if dispatch { "completed" } else { "installed" }, generation,
                    memory.application_outer_head(), memory.application_compute_status());
            }
            Ok(())
        })();
        if let Err(error) = result {
            self.faulted = true;
            if let Some(status) = vm.status() { status.record(error); }
            if let Some(memory) = session.memory.as_mut() {
                for _ in 0..4 { let _ = memory.drain_reports(&session.dev); }
                memory.quarantine_graphics();
                dev_err!(session.dev.as_ref(),
                    "M5_APPLICATION_COMPUTE failed {:?}; outer/graphics={:x?} compute={:x?}\n",
                    error, memory.graphics_status(), memory.application_compute_status());
            }
            let stopped = session.stop_cpus();
            dev_err!(session.dev.as_ref(),
                "M5_APPLICATION_COMPUTE stop={:?}; VM and owners retained until reboot\n", stopped);
        }
        result
    }
}

impl Drop for M5Runtime {
    fn drop(&mut self) {
        if let Some(mut session) = self.session.take() {
            if let Some(memory) = &session.memory { memory.quarantine_graphics(); }
            let stopped = session.stop_cpus();
            dev_dbg!(session.dev.as_ref(),
                "M5 session release requested; stop={:?}; halt unqualified, owners retained until reboot\n",
                stopped);
            core::mem::forget(session);
        }
    }
}
