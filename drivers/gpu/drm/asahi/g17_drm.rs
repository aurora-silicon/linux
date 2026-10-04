// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8140 DRM frontend and userspace render queue.

use core::ops::Range;

use core::sync::atomic::{AtomicU32, Ordering};

use kernel::{
    bindings, c_str,
    dma_fence::{Fence, FenceContexts, FenceObject, FenceOps, RawDmaFence, UserFence},
    macros::vtable,
    new_condvar, new_mutex,
    prelude::*,
    static_lock_class,
    sync::{Arc, CondVar, Mutex},
    time::{delay::fsleep, Delta},
    uapi, xarray,
};

use crate::{
    alloc,
    driver::{AsahiDevRef, AsahiDevice},
    drm_gpu::{DrmGpu, DrmGpuParams},
    file, g17_job, g17_adt_j700, g17_live_boot, g17_manager, g17_render, g17_resources, g17_uapi, gem, gpu, hw, mmu, module_parameters,
    pgtable, queue,
};

const T8140_USER_START: u64 = mmu::UAT_PGSZ as u64;
const T8140_USER_TOP: u64 = 1 << 42;
const T8140_UNKNOWN_PAGE: u64 = T8140_USER_TOP - 2 * mmu::UAT_PGSZ as u64;
const T8140_TIMESTAMP_ALIAS_RANGE: Range<u64> =
    crate::g17_queue_limits::CLIENT_LOW_VA_START..0x71_0000_0000;

/// How long to wait for the firmware to report a compute submission complete.
/// Generous relative to the ~170 GPU ticks a first add3 actually takes; this is
/// a stuck-firmware backstop, not a pacing knob.
const G17P_COMPUTE_COMPLETION_TIMEOUT_MS: u32 = 2000;

/// Most mapped VA runs one submit-time audit will report before it truncates.
const G17P_VM_AUDIT_MAX_RANGES: usize = 64;
/// Hardware context-table entries the audit snapshots: the retained kernel
/// context 0, the single bindable user context 1, and the two contexts a
/// compute kick declares (`T8140_COMPUTE_CTXS` = [2, 3]).
const G17P_VM_AUDIT_CTXS: usize = 4;

pub(crate) type SharedG17PRuntime = Arc<Mutex<g17_live_boot::G17PLiveRuntime>>;

/// Non-owning handle to the platform-owned G17P runtime.
///
/// The platform driver keeps the sole strong reference. DRM registration is
/// removed before that platform state is released, so adapter and queue
/// callbacks cannot outlive this pointer.
#[derive(Clone, Copy)]
pub(crate) struct G17PRuntimeRef {
    ptr: *const Mutex<g17_live_boot::G17PLiveRuntime>,
}

// SAFETY: the referenced mutex is shared by the platform-owned Arc, and all
// access is serialized through that mutex.
unsafe impl Send for G17PRuntimeRef {}
// SAFETY: see the Send implementation above.
unsafe impl Sync for G17PRuntimeRef {}

impl G17PRuntimeRef {
    pub(crate) fn new(runtime: &SharedG17PRuntime) -> Self {
        Self {
            ptr: Arc::as_ptr(runtime),
        }
    }

    pub(crate) fn mutex(&self) -> &Mutex<g17_live_boot::G17PLiveRuntime> {
        // SAFETY: AsahiDriver owns the runtime Arc for the complete registered
        // DRM lifetime. DRM teardown prevents new callbacks before dropping
        // that platform-owned Arc.
        unsafe { &*self.ptr }
    }

    pub(crate) fn service_primary_firmware_events(&self) -> Result {
        self.mutex().lock().service_primary_firmware_events()
    }

    pub(crate) fn service_primary_recovery(&self) -> Result {
        self.mutex().lock().service_primary_recovery()
    }

    pub(crate) fn primary_recovery_pending(&self) -> bool {
        self.mutex().lock().primary_recovery_pending()
    }

    pub(crate) fn primary_firmware_event_pending(&self) -> bool {
        self.mutex().lock().primary_firmware_event_pending()
    }

    /// Give the hardware compute graph back when a client queue goes away.
    fn release_compute_owner(&self, owner: u64) {
        self.mutex().lock().release_compute_owner(owner);
    }

    fn wait_render_slot(&self, render_slot: u8) -> Result {
        for _ in 0..g17_manager::G17PManagerConstruction::RENDER_COMPLETION_POLLS {
            let completion = self
                .mutex()
                .lock()
                .poll_deferred_render_completion(render_slot)?;
            if let Some(completion) = completion {
                return if completion.complete { Ok(()) } else { Err(EIO) };
            }
            fsleep(Delta::from_millis(1));
        }
        self.mutex().lock().abandon_deferred_render(render_slot)?;
        Err(ETIMEDOUT)
    }
}

/// Ceiling on live DRM compute queue objects for this device.
///
/// The firmware's KSM restore set has 128 queue slots, and that is the number
/// of *hardware* QIDs a session could ever carry. This driver configures one
/// compute QID, so the real concurrency limit is the lease below -- one
/// submitting client at a time. The cap exists so a client that leaks queue
/// objects gets a clean `ENOSPC` from QUEUE_CREATE instead of exhausting
/// kernel memory one prepared graph at a time.
const G17P_MAX_COMPUTE_QUEUES: u32 = crate::g17_queue_limits::MAX_QUEUES;

/// Serializes the one hardware compute graph across DRM clients.
///
/// Held after input dependencies become ready, through GPU execution and
/// output-fence publication, including the doorbell wait -- which runs
/// with the runtime mutex released, so the runtime mutex alone does NOT keep
/// two clients out of each other's submission. Also taken when a queue object
/// is destroyed, so a release can never race a submit in flight.
///
/// Uncontended acquisition is one mutex round trip, so the single-client
/// steady-state path (which is the timing-sensitive one) is unchanged.
#[pin_data]
pub(crate) struct G17PQueueRegistry {
    /// Bounded physical ownership: two render pairs plus the one transient
    /// exclusive phase used while installing a compute graph.  Steady-state
    /// compute readers do not occupy either render slot.
    #[pin]
    lease: Mutex<G17PQueueLeaseState>,
    #[pin]
    lease_free: CondVar,
    live_queues: AtomicU32,
    compute_readers: AtomicU32,
    compute_faulted: AtomicU32,
}

#[derive(Default)]
struct G17PQueueLeaseState {
    render_slots: u8,
    compute_install: bool,
}

impl G17PQueueLeaseState {
    const fn render_mask() -> u8 {
        (1u8 << g17_resources::G17PRenderQueuePair::SLOT_COUNT) - 1
    }

    fn acquire_render_slot(&mut self) -> Option<u8> {
        let free = (!self.render_slots) & Self::render_mask();
        if free == 0 { return None; }
        let slot = free.trailing_zeros() as u8;
        self.render_slots |= 1u8 << slot;
        Some(slot)
    }

    fn release_render_slot(&mut self, slot: u8) {
        debug_assert!(slot < g17_resources::G17PRenderQueuePair::SLOT_COUNT);
        self.render_slots &= !(1u8 << slot);
    }
}

impl G17PQueueRegistry {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(G17PQueueRegistry {
                lease <- new_mutex!(G17PQueueLeaseState::default(), "G17PQueueRegistry::lease"),
                lease_free <- new_condvar!("G17PQueueRegistry::lease_free"),
                live_queues: AtomicU32::new(0),
                compute_readers: AtomicU32::new(0),
                compute_faulted: AtomicU32::new(0),
            }),
            GFP_KERNEL,
        )
    }

    /// Independent compute jobs share the compute side of the gate; legacy
    /// render owns it exclusively until its context0 view routing is split.
    /// This protects the qualified render path without serializing compute
    /// queues against each other. The guard lives through exact retirement.
    fn acquire_compute(self: &Arc<Self>, first_install: bool) -> Result<G17PComputeLeaseGuard> {
        let mut held = self.lease.lock();
        while held.compute_install
            || (first_install
                && (held.render_slots != 0
                    || self.compute_readers.load(Ordering::Acquire) != 0))
        {
            if self.compute_faulted.load(Ordering::Acquire) != 0 { return Err(EIO); }
            if self.lease_free.wait_interruptible(&mut held) { return Err(ERESTARTSYS); }
        }
        if self.compute_faulted.load(Ordering::Acquire) != 0 { return Err(EIO); }
        self.compute_readers.fetch_add(1, Ordering::AcqRel);
        if first_install { held.compute_install = true; }
        drop(held);
        Ok(G17PComputeLeaseGuard { registry: self.clone(), exclusive: first_install })
    }

    /// Reserve one queue slot, or report a full device.
    fn reserve_queue(&self) -> Result {
        let mut live = self.live_queues.load(Ordering::Acquire);
        loop {
            if live >= G17P_MAX_COMPUTE_QUEUES {
                return Err(ENOSPC);
            }
            match self.live_queues.compare_exchange(
                live,
                live + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Ok(()),
                Err(observed) => live = observed,
            }
        }
    }

    fn release_queue(&self) {
        self.live_queues.fetch_sub(1, Ordering::AcqRel);
    }

    /// Block until this client owns the hardware compute graph.
    fn acquire_lease(self: &Arc<Self>, _id: u64) -> Result<G17PLeaseGuard> {
        let mut held = self.lease.lock();
        while held.compute_install || held.render_slots == G17PQueueLeaseState::render_mask() {
            if self.compute_faulted.load(Ordering::Acquire) != 0 { return Err(EIO); }
            if self.lease_free.wait_interruptible(&mut held) {
                return Err(ERESTARTSYS);
            }
        }
        if self.compute_faulted.load(Ordering::Acquire) != 0 { return Err(EIO); }
        let slot = held.acquire_render_slot().ok_or(EBUSY)?;
        core::mem::drop(held);
        Ok(G17PLeaseGuard {
            registry: self.clone(),
            slot,
        })
    }
}

pub(crate) struct G17PComputeLeaseGuard { registry: Arc<G17PQueueRegistry>, exclusive: bool }
impl G17PComputeLeaseGuard {
    pub(crate) fn quarantine(&self) {
        let held = self.registry.lease.lock();
        self.registry.compute_faulted.store(1, Ordering::Release);
        drop(held);
        self.registry.lease_free.notify_all();
    }
}
impl Drop for G17PComputeLeaseGuard {
    fn drop(&mut self) {
        let mut held = self.registry.lease.lock();
        if self.exclusive { held.compute_install = false; }
        self.registry.compute_readers.fetch_sub(1, Ordering::AcqRel);
        drop(held);
        self.registry.lease_free.notify_all();
    }
}

/// Releases the compute lease and wakes the next waiting client.
struct G17PLeaseGuard {
    registry: Arc<G17PQueueRegistry>,
    slot: u8,
}

impl G17PLeaseGuard {
    fn slot(&self) -> u8 { self.slot }
}

impl Drop for G17PLeaseGuard {
    fn drop(&mut self) {
        let mut held = self.registry.lease.lock();
        held.release_render_slot(self.slot);
        drop(held);
        self.registry.lease_free.notify_all();
    }
}

const _: () = assert!(!core::mem::needs_drop::<G17PRuntimeRef>());

/// DRM-visible T8140 manager. Hardware lifetime stays in the retained runtime.
pub(crate) struct G17PDrmGpu {
    runtime: G17PRuntimeRef,
    soc: &'static hw::agx3::SocConfig,
    observed: hw::GpuIdConfig,
    ids: gpu::SequenceIDs,
    registry: Arc<G17PQueueRegistry>,
}

impl G17PDrmGpu {
    pub(crate) fn new(
        runtime: G17PRuntimeRef,
        soc: &'static hw::agx3::SocConfig,
        observed: hw::GpuIdConfig,
    ) -> Result<Self> {
        Ok(Self {
            runtime,
            soc,
            observed,
            ids: Default::default(),
            registry: G17PQueueRegistry::new()?,
        })
    }
}

impl DrmGpu for G17PDrmGpu {
    fn init(&self) -> Result {
        Ok(())
    }

    fn ids(&self) -> &gpu::SequenceIDs {
        &self.ids
    }

    fn is_crashed(&self) -> bool {
        // `file.rs::get_params` turns this into ENODEV, and it runs before any
        // ioctl that could reach the submit path's own recovery -- so a crashed
        // session used to lock every LATER process out of the device until the
        // machine was rebooted. Try once (bounded inside) to rebuild instead,
        // then answer with the state that leaves.
        self.runtime.mutex().lock().recover_crashed_session_once()
    }

    fn supports_vm_status(&self) -> bool { true }
    fn independent_queue_limits(&self) -> Option<[u8; 16]> {
        Some(crate::g17_queue_limits::encode_limits())
    }

    fn submission_error(&self) -> i32 {
        if self.registry.compute_faulted.load(Ordering::Acquire) != 0 { EIO.to_errno() } else { 0 }
    }

    fn update_globals(&self) {}

    fn params(&self) -> Result<DrmGpuParams> {
        let mut core_masks = [0u32; uapi::DRM_ASAHI_MAX_CLUSTERS as usize];
        for (index, mask) in self.observed.core_masks.iter().enumerate() {
            *core_masks.get_mut(index).ok_or(EIO)? = *mask;
        }
        Ok(DrmGpuParams {
            gpu_generation: self.observed.gpu_gen as u32,
            gpu_variant: self.observed.gpu_variant as u32,
            gpu_revision: self.observed.gpu_rev as u32,
            chip_id: self.soc.chip_id,
            num_dies: self.observed.num_dies,
            num_clusters_total: self.observed.num_clusters,
            num_cores_per_cluster: self.observed.num_cores,
            core_masks,
            max_frequency_khz: g17_adt_j700::J700_PERF_STATES
                [g17_adt_j700::J700_GPU_NUM_PERF_STATES as usize]
                .freq_hz
                / 1_000,
            usc_generation: self.observed.usc_generation,
            gpu_hal_generation: self.observed.gpu_hal_generation as u32,
            max_commands_per_submission: 1,
        })
    }

    fn user_range(&self) -> Result<Range<u64>> {
        Ok(T8140_USER_START..T8140_UNKNOWN_PAGE)
    }

    fn unknown_page(&self) -> Result<u64> {
        Ok(T8140_UNKNOWN_PAGE)
    }

    fn base_clock_hz(&self) -> u32 {
        24_000_000
    }

    fn new_vm(&self, kernel_range: Range<u64>) -> Result<mmu::Vm> {
        self.runtime
            .mutex()
            .lock()
            .new_user_vm(self.ids.vm.next(), kernel_range)
    }

    fn new_queue(
        &self,
        vm: mmu::Vm,
        _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        _ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        priority: u32,
        usc_exec_base: u64,
    ) -> Result<KBox<dyn queue::Queue>> {
        let dev = self.runtime.mutex().lock().drm_ref()?;
        // Reserved before the queue object exists so the failure is a clean
        // ENOSPC from QUEUE_CREATE. `G17PQueue::drop` gives the slot back.
        self.registry.reserve_queue()?;
        let queue = G17PQueue::new(
            &dev,
            self.runtime,
            self.registry.clone(),
            self.observed.gpc_perf_state_map,
            self.observed.gpc_perf_state_map_low,
            self.observed.gpc_perf_state_control,
            vm,
            self.ids.queue.next(),
            priority,
            usc_exec_base,
        )
        .inspect_err(|_| self.registry.release_queue())?;
        match KBox::new(queue, GFP_KERNEL) {
            Ok(boxed) => Ok(boxed),
            Err(error) => Err(error.into()),
        }
    }

    fn map_timestamp_buffer(
        &self,
        bo: gem::ObjectRef,
        range: Range<usize>,
    ) -> Result<mmu::KernelMapping> {
        self.runtime
            .mutex()
            .lock()
            .map_timestamp_buffer(bo, range)
    }
}

struct UserAddressSpace<'a> {
    vm: &'a mmu::Vm,
    range: Range<u64>,
}

impl g17_uapi::GpuAddressSpace for UserAddressSpace<'_> {
    fn covers(&self, address: u64, size: u64, access: g17_uapi::GpuAccess) -> bool {
        let Some(end) = address.checked_add(size) else {
            return false;
        };
        let (need_read, need_write) = match access {
            g17_uapi::GpuAccess::Read => (true, false),
            g17_uapi::GpuAccess::Write => (false, true),
            g17_uapi::GpuAccess::ReadWrite => (true, true),
        };
        size != 0
            && self.range.start <= address
            && end <= self.range.end
            && self.vm.covers_range(address, size, need_read, need_write)
    }
}

#[derive(Default)]
pub(crate) struct G17PSubmissionFence;

#[vtable]
impl FenceOps for G17PSubmissionFence {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("asahi")
    }

    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("g17p-submit")
    }
}

static QUEUE_NAME: &CStr = c_str!("g17p_queue");
static QUEUE_CLASS_KEY: core::pin::Pin<&'static kernel::sync::LockClassKey> = static_lock_class!();

/// One userspace queue bound to the retained G17P runtime.
///
/// One of these exists per DRM `queue_create`, i.e. per client, and its `id`
/// is what the runtime records as the owner of the hardware compute graph.
/// Dropping it -- `queue_destroy`, or the file close the kernel performs for a
/// client that died mid-submission -- releases that ownership.
struct G17PQueue {
    dev: AsahiDevRef,
    runtime: G17PRuntimeRef,
    registry: Arc<G17PQueueRegistry>,
    /// Cached during the safe SGX GPU-ID probe; never sampled at submission.
    gpc_perf_state_map: u32,
    gpc_perf_state_map_low: u32,
    gpc_perf_state_control: u32,
    vm: mmu::Vm,
    user_range: Range<u64>,
    usc_exec_base: u64,
    fence_contexts: FenceContexts,
    async_backend: Option<Arc<G17PAsyncQueueBackend>>,
    async_scheduler: Option<g17_job::G17PJobScheduler<G17PAsyncQueueBackend>>,
    /// Most recent compute job submitted through this logical DRM queue.
    /// Render still uses the synchronous G17P path, so it must explicitly
    /// join the scheduler timeline before acquiring the render lease.
    last_async_compute: Option<Fence>,
    id: u64,
}

impl Drop for G17PQueue {
    fn drop(&mut self) {
        // Entity teardown handles pending scheduler jobs before the frontend
        // reference goes away. Active hardware packets are retained by runtime.
        drop(self.async_scheduler.take());
        drop(self.async_backend.take());
        self.runtime.release_compute_owner(self.id);
        self.registry.release_queue();
    }
}

enum G17PPendingCommand {
    Render(g17_uapi::TranslatedRenderCommand),
    Compute(g17_uapi::TranslatedComputeCommand),
}

type TimestampDestination = g17_job::TimestampDestination;

pub(crate) struct G17PAsyncQueueBackend {
    runtime: G17PRuntimeRef,
    registry: Arc<G17PQueueRegistry>,
    owner: u64,
    hardware_queue: u8,
    context: Arc<mmu::T8140ComputeExecutionContext>,
}

impl Drop for G17PAsyncQueueBackend {
    fn drop(&mut self) { self.runtime.mutex().lock().release_async_compute_queue(self.owner); }
}

impl g17_job::G17PQueueBackend for G17PAsyncQueueBackend {
    fn prepare(&self, _packet: &g17_job::G17PComputeJobPacket) -> Option<Fence> {
        self.runtime.mutex().lock().async_compute_dependency(self.owner)
    }
    fn publish(&self, packet: Arc<g17_job::G17PComputeJobPacket>) -> Result<Fence> {
        let hardware_fence = packet.completion()?.fence();
        let first_install = self.runtime.mutex().lock().async_compute_needs_install(self.owner)?;
        let lease = self.registry.acquire_compute(first_install)?;
        self.runtime.mutex().lock().publish_async_compute(self.owner, packet, lease)?;
        Ok(hardware_fence)
    }
    fn timed_out(&self, packet: Arc<g17_job::G17PComputeJobPacket>) -> kernel::drm::sched::Status {
        self.runtime.mutex().lock().quarantine_async_compute(self.owner, packet.submission_id(), ETIMEDOUT);
        kernel::drm::sched::Status::Nominal
    }
    fn cancel(&self, packet: Arc<g17_job::G17PComputeJobPacket>) {
        let active = self.runtime.mutex().lock()
            .quarantine_async_compute(self.owner, packet.submission_id(), ECANCELED);
        if !active {
            if let Ok(completion) = packet.completion() { completion.complete(Err(ECANCELED)); }
        }
    }
}

impl G17PQueue {
    fn new(
        dev: &AsahiDevice,
        runtime: G17PRuntimeRef,
        registry: Arc<G17PQueueRegistry>,
        gpc_perf_state_map: u32,
        gpc_perf_state_map_low: u32,
        gpc_perf_state_control: u32,
        vm: mmu::Vm,
        id: u64,
        priority: u32,
        usc_exec_base: u64,
    ) -> Result<Self> {
        if priority > 3 {
            return Err(EINVAL);
        }
        let user_range = T8140_USER_START..T8140_UNKNOWN_PAGE;
        g17_uapi::QueueUscWindow {
            base: usc_exec_base,
            user_start: user_range.start,
            user_end: user_range.end,
        }
        .validate()
        .map_err(|_| EINVAL)?;
        let fence_contexts = FenceContexts::new(1, QUEUE_NAME, QUEUE_CLASS_KEY)?;
        // Reserve all scarce hardware resources before QUEUE_CREATE succeeds.
        // Allocation errors must not surface later as accepted-job device loss.
        // Backend Drop gives an unpublished/idle reservation back on any later
        // constructor failure, including scheduler or KBox allocation failure.
        let (hardware_queue, context) = runtime.mutex().lock()
            .prepare_async_compute_queue(id, &vm)?;
        let backend = Arc::new(G17PAsyncQueueBackend {
            runtime, registry: registry.clone(), owner: id, hardware_queue, context,
        }, GFP_KERNEL)?;
        let scheduler = g17_job::G17PJobScheduler::new(dev, backend.clone(),
            crate::g17_queue_limits::MAX_IN_FLIGHT_PER_QUEUE)?;
        Ok(Self {
            dev: dev.into(),
            runtime,
            registry,
            gpc_perf_state_map,
            gpc_perf_state_map_low,
            gpc_perf_state_control,
            vm,
            user_range,
            usc_exec_base,
            fence_contexts,
            async_backend: Some(backend),
            async_scheduler: Some(scheduler),
            last_async_compute: None,
            id,
        })
    }

    /// Name the UAPI check that refused a submission. Without this the ioctl
    /// only reports a bare EINVAL, and every validation in `g17_uapi` looks
    /// alike from userspace.
    fn reject_uapi(
        submission_id: u64,
        stage: &str,
        error: g17_uapi::UapiTranslateError,
    ) -> kernel::error::Error {
        pr_info!(
            "G17P submit: UAPI REJECT submission {} at {} check={}\n",
            submission_id,
            stage,
            error.name()
        );
        EINVAL
    }

    /// Log and return any error raised on the submit path outside UAPI
    /// translation. `reject_uapi` only covers parse/translate; everything from
    /// the sync-count check to the manager publish used to fail silently, which
    /// is why a submission could die with a bare EINVAL and no marker at all.
    fn stage_fail(submission_id: u64, stage: &str, error: kernel::error::Error) -> kernel::error::Error {
        pr_info!(
            "G17P submit: FAIL submission {} stage={} errno={}\n",
            submission_id,
            stage,
            error.to_errno()
        );
        error
    }

    /// Walk the raw command buffer and report every command header, without
    /// validating anything. This says what a submission actually CONTAINS --
    /// render vs compute vs attachment-set, sizes and barriers -- so a
    /// submission that dies before the parse loop is still identifiable.
    fn log_cmdbuf_headers(submission_id: u64, raw: &[u8]) {
        let mut offset = 0usize;
        let mut index = 0u32;
        while offset + g17_uapi::UAPI_COMMAND_HEADER_SIZE <= raw.len() {
            let header = &raw[offset..offset + g17_uapi::UAPI_COMMAND_HEADER_SIZE];
            let command_type = u16::from_le_bytes([header[0], header[1]]);
            let size = u16::from_le_bytes([header[2], header[3]]);
            let vdm_barrier = u16::from_le_bytes([header[4], header[5]]);
            let cdm_barrier = u16::from_le_bytes([header[6], header[7]]);
            pr_info!(
                "G17P submit: submission {} cmd[{}] type={} size={} vdm_barrier={:#x} cdm_barrier={:#x} at offset {}\n",
                submission_id,
                index,
                command_type,
                size,
                vdm_barrier,
                cdm_barrier,
                offset,
            );
            offset += g17_uapi::UAPI_COMMAND_HEADER_SIZE + size as usize;
            index += 1;
            if index > 8 {
                pr_info!("G17P submit: submission {} cmd list truncated\n", submission_id);
                return;
            }
        }
        if offset != raw.len() {
            pr_info!(
                "G17P submit: submission {} cmd walk ended at {} of {} bytes (ragged)\n",
                submission_id,
                offset,
                raw.len(),
            );
        }
    }

    fn submit_compute_async(
        &mut self, submission_id: u64, translated: g17_uapi::TranslatedComputeCommand,
        mut syncs: KVec<file::SyncItem>, in_sync_count: usize,
        objects: core::pin::Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        self.runtime.mutex().lock().check_async_compute_submission()?;
        let destinations = [
            Self::timestamp_destination(objects, translated.timestamps.start)?,
            Self::timestamp_destination(objects, translated.timestamps.end)?,
        ];
        let timestamps = self.owned_compute_timestamps()?;
        let output = self.fence_contexts.new_fence(0, G17PSubmissionFence)?.into();
        let backend = self.async_backend.as_ref().ok_or(EIO)?;
        let packet = g17_job::G17PComputeJobPacket::new(
            submission_id, backend.hardware_queue, backend.context.clone(), translated,
            KVec::new(), KVec::new(),
            Some(g17_job::G17PJobCompletion { fence: output, timestamps, destinations, status: self.vm.status().clone() }),
            Some(self.vm.retain_t8140_job()?),
        )?;
        let mut dependencies = KVec::with_capacity(in_sync_count, GFP_KERNEL)?;
        for sync in syncs.drain(0..in_sync_count) {
            if let Some(fence) = sync.fence { dependencies.push(fence, GFP_KERNEL)?; }
        }
        let finished = self.async_scheduler.as_mut().ok_or(EIO)?.enqueue(packet, dependencies)?;
        Self::install_output_fences(syncs.into_iter(), &finished);
        self.last_async_compute = Some(finished);
        pr_info!("G17P async submit: QUEUED submission {} owner={} qid={} context={}\n",
            submission_id, self.id, backend.hardware_queue, backend.context.context_id());
        Ok(())
    }

    /// Dump the render payload exactly as userspace supplied it, before any
    /// validation runs. A rejected submit never reaches the GPU, so this is
    /// the only place the offending field is visible -- and printing it
    /// unconditionally means one hardware run diagnoses any check, not just
    /// the one that happens to fire first.
    fn log_render_payload(submission_id: u64, payload: &g17_uapi::UapiRenderCommand) {
        pr_info!(
            "G17P render uapi[{}]: flags={:#x} {}x{} layers={} utile={}x{} samples={} sample_size={} tib_cfg={:#x}\n",
            submission_id,
            payload.flags,
            payload.width,
            payload.height,
            payload.layers,
            payload.utile_width,
            payload.utile_height,
            payload.samples,
            payload.sample_size,
            payload.ppp_control,
        );
        pr_info!(
            "G17P render uapi[{}]: vdm={:#x} scissor={:#x} dbias={:#x} oclqry={:#x} sampler={:#x}/{} bgobjdepth={:#x} bgobjvals={:#x}\n",
            submission_id,
            payload.vdm_base,
            payload.scissor_base,
            payload.depth_bias_base,
            payload.occlusion_query_base,
            payload.sampler_heap,
            payload.sampler_count,
            payload.depth_clear,
            payload.stencil_clear,
        );
        pr_info!(
            "G17P render uapi[{}]: depth={:#x}/{:#x} stride={:#x}/{:#x} stencil={:#x}/{:#x} stride={:#x}/{:#x} zls={:#x}\n",
            submission_id,
            payload.depth.base,
            payload.depth.compression_base,
            payload.depth.stride,
            payload.depth.compression_stride,
            payload.stencil.base,
            payload.stencil.compression_base,
            payload.stencil.stride,
            payload.stencil.compression_stride,
            payload.zls_control,
        );
        pr_info!(
            "G17P render uapi[{}]: bg={:#x}/{:#x} eot={:#x}/{:#x} pbg={:#x}/{:#x} peot={:#x}/{:#x}\n",
            submission_id,
            payload.background.usc,
            payload.background.resource_spec,
            payload.end_of_tile.usc,
            payload.end_of_tile.resource_spec,
            payload.partial_background.usc,
            payload.partial_background.resource_spec,
            payload.partial_end_of_tile.usc,
            payload.partial_end_of_tile.resource_spec,
        );
        pr_info!(
            "G17P render uapi[{}]: vhelper={:#x}/{:#x}/{:#x} fhelper={:#x}/{:#x}/{:#x}\n",
            submission_id,
            payload.vertex_helper.binary,
            payload.vertex_helper.config,
            payload.vertex_helper.data,
            payload.fragment_helper.binary,
            payload.fragment_helper.config,
            payload.fragment_helper.data,
        );
    }

    fn wait_input_fences(syncs: &[file::SyncItem]) -> Result {
        for sync in syncs {
            let Some(fence) = sync.fence.as_ref() else {
                continue;
            };
            // SAFETY: `fence.raw()` remains valid for the duration of this call.
            let ret = unsafe {
                bindings::dma_fence_wait_timeout(
                    fence.raw(),
                    true,
                    kernel::task::MAX_SCHEDULE_TIMEOUT,
                )
            };
            if ret < 0 {
                return Err(Error::from_errno(ret as i32));
            }
        }
        Ok(())
    }

    fn acquire_ready_lease(
        &self, submission_id: u64, syncs: &[file::SyncItem],
    ) -> Result<G17PLeaseGuard> {
        Self::wait_input_fences(syncs)
            .map_err(|error| Self::stage_fail(submission_id, "wait-input-fences", error))?;
        self.registry.acquire_lease(self.id)
            .map_err(|error| Self::stage_fail(submission_id, "acquire-lease", error))
    }

    fn join_async_compute_timeline(&mut self, submission_id: u64) -> Result {
        let Some(fence) = self.last_async_compute.as_ref() else {
            return Ok(());
        };
        // SAFETY: the queue owns `fence` until this wait returns.
        let ret = unsafe {
            bindings::dma_fence_wait_timeout(
                fence.raw(),
                true,
                kernel::task::MAX_SCHEDULE_TIMEOUT,
            )
        };
        if ret < 0 {
            return Err(Self::stage_fail(
                submission_id,
                "wait-queue-compute",
                Error::from_errno(ret as i32),
            ));
        }
        self.last_async_compute = None;
        if self.vm.status().get() != 0 { return Err(EIO); }
        Ok(())
    }

    fn timestamp_address(
        objects: core::pin::Pin<&xarray::XArray<KBox<file::Object>>>,
        vm: &mmu::Vm,
        timestamp: g17_uapi::UapiTimestamp,
        aliases: &mut KVec<mmu::KernelMapping>,
    ) -> Result<u64> {
        if timestamp.handle == 0 {
            return Ok(0);
        }
        let guard = objects.lock();
        let object = guard
            .get(timestamp.handle.try_into()?)
            .ok_or(ENOENT)?
            .clone();
        core::mem::drop(guard);
        match object {
            file::Object::TimestampBuffer(mapping) => {
                let end = timestamp.offset.checked_add(8).ok_or(EINVAL)? as usize;
                if end > mapping.size() {
                    return Err(ERANGE);
                }
                let alias = mapping.map_alias_into_range(
                    vm,
                    T8140_TIMESTAMP_ALIAS_RANGE.clone(),
                    mmu::PROT_GPU_FW_SHARED_RW,
                )?;
                let address = alias
                    .iova()
                    .checked_add(timestamp.offset as u64)
                    .ok_or(EINVAL)?;
                aliases.push(alias, GFP_KERNEL)?;
                Ok(address)
            }
        }
    }

    fn timestamp_destination(
        objects: core::pin::Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: g17_uapi::UapiTimestamp,
    ) -> Result<Option<TimestampDestination>> {
        if timestamp.handle == 0 {
            return Ok(None);
        }
        let guard = objects.lock();
        let object = guard
            .get(timestamp.handle.try_into()?)
            .ok_or(ENOENT)?
            .clone();
        core::mem::drop(guard);
        match object {
            file::Object::TimestampBuffer(mapping) => {
                let offset: usize = timestamp.offset.try_into()?;
                let end = offset.checked_add(8).ok_or(EINVAL)?;
                if end > mapping.size() {
                    return Err(ERANGE);
                }
                Ok(Some(TimestampDestination { mapping, offset }))
            }
        }
    }

    fn copy_timestamp(destination: Option<TimestampDestination>, value: u64) -> Result {
        let Some(destination) = destination else {
            return Ok(());
        };
        destination.mapping.with_cpu_bytes(|raw| {
            let end = destination.offset.checked_add(8).ok_or(EINVAL)?;
            let bytes = raw.get_mut(destination.offset..end).ok_or(ERANGE)?;
            bytes.copy_from_slice(&value.to_le_bytes());
            core::sync::atomic::fence(core::sync::atomic::Ordering::Release);
            Ok(())
        })
    }

    fn owned_compute_timestamps(&self) -> Result<mmu::KernelMapping> {
        // Firmware writes both completion words. A WB CPU alias can keep the
        // polling core on stale zeroes even after the GPU has completed.
        let mut object = gem::new_kernel_object_wc(&self.dev, mmu::UAT_PGSZ)?;
        object.vmap()?.memset(0);
        object.map_into_range(
            &self.vm,
            T8140_TIMESTAMP_ALIAS_RANGE.clone(),
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
            false,
        )
    }

    fn install_output_fences(mut syncs: impl Iterator<Item = file::SyncItem>, fence: &Fence) {
        for mut sync in syncs.by_ref() {
            if let Some(chain) = sync.chain_fence.take() {
                sync.syncobj.add_point(chain, fence, sync.timeline_value);
            } else {
                sync.syncobj.replace_fence(Some(fence));
            }
        }
    }
}

/// Submit-time reachability audit.
///
/// A `reason=3` GMMU page fault on a pointer the driver has already checked
/// with `covers_range` leaves three things unexplained, and none of them can
/// be read from the sgx MMU fault bank -- that bank is a reset-default phantom
/// on G17P, and touching sgx MMIO once the cores have gated takes an
/// asynchronous SError and panics the machine.  Everything below reads host
/// DRAM only (this VM's page tables and the TTBAT region), so it is safe in
/// exactly the state a faulted submission leaves the GPU in.
impl G17PQueue {
    /// Compare a TTBAT entry against a page-table root.
    ///
    /// TTB0 carries the root physical address plus a valid bit and the
    /// context's ASID, so only the address field is comparable.  The mask is
    /// the firmware's own declared level mask (`g17_initdata::UAT_LEVEL_PHYS_MASK`).
    const TTB_ADDR_MASK: u64 = 0x0000_03ff_ffff_c000;

    fn log_hex(label: &str, base: u64, bytes: &[u8]) {
        for (index, chunk) in bytes.chunks(16).enumerate() {
            pr_info!(
                "G17P vm-audit: {} {:#x} {:02x?}\n",
                label,
                base.wrapping_add((index * 16) as u64),
                chunk,
            );
        }
    }

    fn log_shader_from_ldshdr(
        &self,
        ioto: &[u8],
        probe: &mut impl FnMut(&str, u64, u64, bool),
    ) {
        let word = |offset: usize| -> u64 {
            let mut raw = [0u8; 2];
            raw.copy_from_slice(&ioto[offset..offset + 2]);
            u16::from_le_bytes(raw) as u64
        };
        let mut found = None;
        let mut offset = 0usize;
        while offset + 10 <= ioto.len() {
            if ioto[offset] == 0x77 && ioto[offset + 1] == 0x01 {
                found = Some(offset);
                break;
            }
            offset += 2;
        }
        let Some(offset) = found else {
            pr_info!("G17P vm-audit: no LDSHDR (77 01) record in the program\n");
            return;
        };
        let w1 = word(offset + 2);
        let w2 = word(offset + 4);
        let w3 = word(offset + 6);
        let w4 = word(offset + 8);
        let shader_va = (((w1 >> 7) & 0x1ff) << 6) | (w2 << 15) | (w3 << 31) | ((w4 & 1) << 47);
        // e4 = {2, 0, 1, 3}; invert it to recover the program kind.
        let kind = match (w4 >> 1) & 3 {
            2 => 0,
            0 => 1,
            1 => 2,
            _ => 3,
        };
        pr_info!(
            "G17P vm-audit: ldshdr at +{:#x} shader_va={:#x} kind={} words={:#06x} {:#06x} {:#06x} {:#06x}\n",
            offset,
            shader_va,
            kind,
            w1,
            w2,
            w3,
            w4,
        );
        if shader_va == 0 {
            return;
        }

        // Is anything mapped in front of the entry point? add3's entry sits
        // 0x3c0 bytes into a container whose header dwords live at +0 and at
        // entry-0x80; ours is at offset 0 of a bare allocation.
        probe("shader_entry-0x80", shader_va.wrapping_sub(0x80), 0x80, false);
        probe("shader_entry-0x3c0", shader_va.wrapping_sub(0x3c0), 0x3c0, false);
        probe("shader_binary", shader_va, 192, false);

        let container = shader_va.wrapping_sub(0x3c0);
        let mut lead = [0u8; 64];
        match self.vm.read_bytes(container, &mut lead) {
            Ok(()) => Self::log_hex("container", container, &lead),
            Err(error) => {
                pr_info!("G17P vm-audit: container read failed ({:?})\n", error)
            }
        }
        for offset in [0x100u64, 0x200u64] {
            let mut table = [0u8; 32];
            match self.vm.read_bytes(container.wrapping_add(offset), &mut table) {
                Ok(()) => Self::log_hex(
                    "container-helpers",
                    container.wrapping_add(offset),
                    &table,
                ),
                Err(error) => pr_info!(
                    "G17P vm-audit: container +{:#x} read failed ({:?})\n",
                    offset,
                    error
                ),
            }
        }
        // Block dword, then the constant program, right up to the entry.
        let mut block = [0u8; 128];
        match self.vm.read_bytes(container.wrapping_add(0x340), &mut block) {
            Ok(()) => {
                let mut raw = [0u8; 4];
                raw.copy_from_slice(&block[0..4]);
                pr_info!(
                    "G17P vm-audit: container block dword = {:#x} (witness carries 0xc0 for a 56-byte shader)\n",
                    u32::from_le_bytes(raw)
                );
                Self::log_hex("container-block", container.wrapping_add(0x340), &block);
            }
            Err(error) => pr_info!(
                "G17P vm-audit: container block read failed ({:?})\n",
                error
            ),
        }

        let mut shader = [0u8; 192];
        match self.vm.read_bytes(shader_va, &mut shader) {
            Ok(()) => Self::log_hex("shader", shader_va, &shader),
            Err(error) => {
                pr_info!("G17P vm-audit: shader read failed ({:?})\n", error)
            }
        }
    }

    fn log_vm_pointer_audit(&self, command: &g17_uapi::TranslatedComputeCommand) {
        if *module_parameters::g17p_fault_report.value() == 0 {
            return;
        }

        let root = self.vm.page_table_root();
        pr_info!(
            "G17P vm-audit: vm root={:#x} user-range={:#x}..{:#x} usc-base={:#x}\n",
            root,
            self.user_range.start,
            self.user_range.end,
            self.usc_exec_base,
        );

        // 1. Does the hardware context the compute kick declares actually name
        //    this VM's page-table root?  `install_t8140_compute_context_alias`
        //    publishes contexts 2 and 3 as aliases of the slot-1 binding; if
        //    either still names an earlier root the GPU is walking somebody
        //    else's tables and every pointer below is irrelevant.
        let mut ctxs = [(0u64, 0u64); G17P_VM_AUDIT_CTXS];
        match self.vm.context_roots(&mut ctxs) {
            Ok(()) => {
                for (slot, (ttb0, ttb1)) in ctxs.iter().enumerate() {
                    let names_this_vm =
                        (ttb0 & Self::TTB_ADDR_MASK) == (root & Self::TTB_ADDR_MASK);
                    pr_info!(
                        "G17P vm-audit(pre-bind): ctx {} ttb0={:#x} ttb1={:#x} names-this-vm={}\n",
                        slot,
                        ttb0,
                        ttb1,
                        names_this_vm,
                    );
                    // Contexts 2 and 3 are the ones a compute kick declares.
                    if (slot == 2 || slot == 3) && !names_this_vm {
                        pr_info!(
                            "G17P vm-audit(pre-bind): compute context {} does not name this VM's root yet ({:#x} vs {:#x}). EXPECTED on a first bind -- this sample is taken BEFORE bind_compute_user_vm publishes the alias. The decisive line is G17P vm-audit(pre-kick).\n",
                            slot,
                            ttb0,
                            root,
                        );
                    }
                }
            }
            Err(error) => pr_info!(
                "G17P vm-audit: context-root snapshot failed ({:?})\n",
                error
            ),
        }

        // 2. Every pointer the UAPI hands the GPU, checked against the tables
        //    the GPU actually walks.  `l1` is the top-level page-table index
        //    (VA >> 36 for the 42-bit AGX3 root), which is what a per-entry
        //    aliasing gap would sort by.
        let mut probe = |name: &str, address: u64, size: u64, write: bool| {
            if address == 0 || size == 0 {
                return;
            }
            let covered = self.vm.covers_range(address, size, true, write);
            let phys = self.vm.translate_iova(address).unwrap_or(0);
            pr_info!(
                "G17P vm-audit: {} {:#x}+{:#x} covered={} phys={:#x} l1={:#x}\n",
                name,
                address,
                size,
                covered,
                phys,
                address >> 36,
            );
        };

        probe(
            "usc_exec_base",
            command.usc_exec_base,
            mmu::UAT_PGSZ as u64,
            false,
        );
        probe(
            "cdm_stream",
            command.control_stream_base,
            command
                .control_stream_end
                .saturating_sub(command.control_stream_base),
            false,
        );
        if command.sampler_count != 0 {
            probe(
                "sampler_heap",
                command.sampler_heap,
                command.sampler_count as u64 * 8,
                false,
            );
        }
        for (index, entry) in command.attachments.as_slice().iter().enumerate() {
            let _ = index;
            probe("attachment", entry.address, entry.size, true);
        }
        if let Some(buffers) = command.g17p_add3_buffers {
            probe("add3_input_a", buffers[0], 256, false);
            probe("add3_input_b", buffers[1], 256, false);
            probe("add3_output", buffers[2], 256, true);
        }

        probe(
            "usc_resource+0xf8000",
            command.usc_exec_base.wrapping_add(0xf_8000),
            mmu::UAT_PGSZ as u64,
            false,
        );

        // Follow the command stream the way the GPU does.
        //
        // The UAPI names only the control-stream range. `program_va` lives
        // inside the launch record, and every other pointer the GPU follows
        // lives inside the Ioto program that record names -- so the only way
        // to enumerate them is to read them. A G17P GMMU fault names no
        // address (the MMIO bank is a reset-default phantom), which makes this
        // the only route to "which pointer was not mapped".
        let cs_len = command
            .control_stream_end
            .saturating_sub(command.control_stream_base);
        if cs_len >= 44 {
            let mut cdm = [0u8; 64];
            let take = core::cmp::min(cs_len as usize, cdm.len());
            match self
                .vm
                .read_bytes(command.control_stream_base, &mut cdm[..take])
            {
                Ok(()) => {
                    Self::log_hex("cdm", command.control_stream_base, &cdm[..take]);
                    let word = |index: usize| -> u32 {
                        let mut raw = [0u8; 4];
                        raw.copy_from_slice(&cdm[index * 4..index * 4 + 4]);
                        u32::from_le_bytes(raw)
                    };
                    // pipeline = ((word1 >> 22) << 38) | (word2 << 6)
                    let program_va =
                        (((word(1) as u64) >> 22) << 38) | ((word(2) as u64) << 6);
                    pr_info!(
                        "G17P vm-audit: launch words={:08x} {:08x} {:08x} {:08x} grid=({},{},{}) local=({},{},{}) tail={:08x} program_va={:#x}\n",
                        word(0),
                        word(1),
                        word(2),
                        word(3),
                        word(4),
                        word(5),
                        word(6),
                        word(7),
                        word(8),
                        word(9),
                        word(10),
                        program_va,
                    );
                    probe("ioto_program", program_va, 256, false);
                    let mut ioto = [0u8; 256];
                    match self.vm.read_bytes(program_va, &mut ioto) {
                        Ok(()) => {
                            Self::log_hex("ioto", program_va, &ioto);
                            self.log_shader_from_ldshdr(&ioto, &mut probe);
                        }
                        Err(error) => {
                            pr_info!("G17P vm-audit: ioto read failed ({:?})\n", error)
                        }
                    }
                }
                Err(error) => pr_info!("G17P vm-audit: cdm read failed ({:?})\n", error),
            }
        }

        // Chase the descriptor chain the SHADER walks.
        //
        // Nothing above can reach it: the root descriptor's address is an
        // LDIMM immediate inside the Ioto program, and what it points AT --
        // `sets[0]`, and inside that the buffer address the shader finally
        // stores through -- exists only as data. A store through a bad one of
        // those is the only remaining way this submission can take a GMMU
        // fault once every driver- and Mesa-named pointer is mapped, and it is
        // invisible to every other check. Point `g17p_vm_probe_va` at Mesa's
        // `G17P root: va=` value and this walks one level for you.
        let probe_va = *module_parameters::g17p_vm_probe_va.value();
        if probe_va != 0 {
            let mut head = [0u8; 64];
            match self.vm.read_bytes(probe_va, &mut head) {
                Ok(()) => {
                    Self::log_hex("probe", probe_va, &head);
                    for index in 0..(head.len() / 8) {
                        let mut raw = [0u8; 8];
                        raw.copy_from_slice(&head[index * 8..index * 8 + 8]);
                        let candidate = u64::from_le_bytes(raw);
                        if candidate < self.user_range.start
                            || candidate >= self.user_range.end
                        {
                            continue;
                        }
                        probe("probe-chain", candidate, 32, false);
                        let mut leaf = [0u8; 32];
                        match self.vm.read_bytes(candidate, &mut leaf) {
                            Ok(()) => Self::log_hex("probe-chain", candidate, &leaf),
                            Err(error) => pr_info!(
                                "G17P vm-audit: probe-chain {:#x} read failed ({:?})\n",
                                candidate,
                                error
                            ),
                        }
                    }
                }
                Err(error) => pr_info!(
                    "G17P vm-audit: probe {:#x} read failed ({:?})\n",
                    probe_va,
                    error
                ),
            }
        }

        // 3. Every VA run that exists in this VM, so a pointer userspace
        //    believes it bound can be checked mechanically instead of guessed
        //    at from a fault address.
        let mut ranges = [pgtable::MappedRange::default(); G17P_VM_AUDIT_MAX_RANGES];
        match self.vm.mapped_ranges(self.user_range.clone(), &mut ranges) {
            Ok((count, truncated)) => {
                pr_info!(
                    "G17P vm-audit: {} mapped run(s){}\n",
                    count,
                    if truncated { " TRUNCATED" } else { "" },
                );
                for run in ranges.iter().take(count) {
                    pr_info!(
                        "G17P vm-audit:   {:#x}..{:#x} ({:#x}) pte={:#x} l1={:#x}\n",
                        run.start,
                        run.end,
                        run.end - run.start,
                        run.pte,
                        run.start >> 36,
                    );
                }
            }
            Err(error) => pr_info!("G17P vm-audit: range walk failed ({:?})\n", error),
        }
    }
}

impl queue::Queue for G17PQueue {
    fn submit(
        &mut self,
        submission_id: u64,
        mut syncs: KVec<file::SyncItem>,
        in_sync_count: usize,
        cmdbuf_raw: &[u8],
        objects: core::pin::Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        if self.vm.status().get() != 0 { return Err(EIO); }
        pr_info!(
            "G17P submit: ENTER submission {} syncs={} in_sync={} cmdbuf_bytes={}\n",
            submission_id,
            syncs.len(),
            in_sync_count,
            cmdbuf_raw.len()
        );
        Self::log_cmdbuf_headers(submission_id, cmdbuf_raw);
        if in_sync_count > syncs.len() {
            return Err(Self::stage_fail(submission_id, "sync-count", EINVAL));
        }
        let address_space = UserAddressSpace {
            vm: &self.vm,
            range: self.user_range.clone(),
        };
        let window = g17_uapi::QueueUscWindow {
            base: self.usc_exec_base,
            user_start: self.user_range.start,
            user_end: self.user_range.end,
        };
        let mut parser = g17_uapi::UapiCommandParser::new(cmdbuf_raw);
        let mut pending = None;
        while let Some(command) = parser
            .next_hardware()
            .map_err(|error| Self::reject_uapi(submission_id, "parse", error))?
        {
            if pending.is_some() {
                return Err(ENOTSUPP);
            }
            pending = Some(match command {
                g17_uapi::ParsedHardwareCommand::Render {
                    payload,
                    vertex_attachments,
                    fragment_attachments,
                    ..
                } => {
                    Self::log_render_payload(submission_id, &payload);
                    // Dump Mesa's VDM control stream. The tiler provably
                    // FETCHES it -- pipe-vdm-stream reads back the encoded
                    // pointer on pipes 0 and 2 -- and then bins 60 non-zero
                    // bytes into a 1.28 MB tile buffer, whose content is an
                    // empty bounding box (0x7fc00000 NaN and 0x7fffffff
                    // extremes). So the stream is reached and executed and
                    // yields zero primitives. Its first bytes say whether it
                    // contains a draw at all.
                    if *module_parameters::g17p_vdm_dump.value() != 0 {
                        let mut head = [0u8; 1024];
                        match self.vm.read_bytes(payload.vdm_base, &mut head) {
                            Ok(()) => Self::log_hex("vdm", payload.vdm_base, &head),
                            Err(error) => pr_info!(
                                "G17P vdm dump: read {:#x} failed ({:?})\n",
                                payload.vdm_base,
                                error
                            ),
                        }
                        // g17p_vm_probe_va was compute-only. The render path
                        // needs it more: the VDM stream embeds the vertex
                        // buffer address, and NaN bounds in the tile buffer
                        // point at what the shader READS rather than at the
                        // stream itself.
                        let probe = *module_parameters::g17p_vm_probe_va.value();
                        if probe != 0 {
                            let mut buf = [0u8; 64];
                            match self.vm.read_bytes(probe, &mut buf) {
                                Ok(()) => Self::log_hex("vdm-probe", probe, &buf),
                                Err(error) => pr_info!(
                                    "G17P vdm probe: read {:#x} failed ({:?})\n",
                                    probe,
                                    error
                                ),
                            }
                        }
                    }
                    G17PPendingCommand::Render(
                        g17_uapi::translate_render_command(
                            payload,
                            vertex_attachments,
                            fragment_attachments,
                            window,
                            &address_space,
                            g17_uapi::RenderInternalState {
                                parameters: {
                                    let mut p =
                                        g17_render::G17pRenderParameters::source_rsrc8();
                                    if *module_parameters::g17p_render_bg_prefix.value() != 0 {
                                        p.bg_resource_prefix = 0xffff_8000_0000_0000;
                                    }
                                    p.native_pm_bytes =
                                        crate::g17_resources::g17p_render_native_pm_bytes();
                                    p.native_ta_registers =
                                        *module_parameters::g17p_render_native_regs.value();
                                    p.gpc_perf_state_map = self.gpc_perf_state_map;
                                    p.gpc_perf_state_map_low = self.gpc_perf_state_map_low;
                                    p.gpc_perf_state_control = self.gpc_perf_state_control;
                                    p
                                },
                            },
                        )
                        .map_err(|error| Self::reject_uapi(submission_id, "render", error))?,
                    )
                }
                g17_uapi::ParsedHardwareCommand::Compute {
                    payload,
                    attachments,
                    ..
                } => G17PPendingCommand::Compute(
                    g17_uapi::translate_compute_command(
                        payload,
                        attachments,
                        window,
                        &address_space,
                    )
                    .map_err(|error| Self::reject_uapi(submission_id, "compute", error))?,
                ),
            });
        }
        parser
            .finish()
            .map_err(|error| Self::reject_uapi(submission_id, "finish", error))?;
        let pending = pending.ok_or_else(|| {
            Self::stage_fail(submission_id, "no-hardware-command", EINVAL)
        })?;
        pr_info!(
            "G17P submit: submission {} translated as {}\n",
            submission_id,
            match pending {
                G17PPendingCommand::Render(_) => "render",
                G17PPendingCommand::Compute(_) => "compute",
            }
        );

        if let G17PPendingCommand::Compute(translated) = pending {
            return self.submit_compute_async(submission_id, translated, syncs, in_sync_count, objects);
        }

        self.join_async_compute_timeline(submission_id)?;

        // Dependencies must become ready before reserving the physical
        // queues: their signalling submission may belong to another client.
        // The lease then spans timestamp publication, execution/doorbell waits
        // and output-fence installation, including runtime-mutex release.
        let lease = self.acquire_ready_lease(submission_id, &syncs[..in_sync_count])?;
        let output_fence: UserFence<G17PSubmissionFence> = self
            .fence_contexts
            .new_fence(0, G17PSubmissionFence)
            .map_err(|error| Self::stage_fail(submission_id, "new-fence", error))?
            .into();
        match pending {
            G17PPendingCommand::Render(translated) => {
                let mut timestamp_aliases = KVec::new();
                let timestamps = [
                    Self::timestamp_address(
                        objects,
                        &self.vm,
                        translated.vertex_timestamps.start,
                        &mut timestamp_aliases,
                    )
                    .map_err(|error| Self::stage_fail(submission_id, "ts-vtx-start", error))?,
                    Self::timestamp_address(
                        objects,
                        &self.vm,
                        translated.vertex_timestamps.end,
                        &mut timestamp_aliases,
                    )
                    .map_err(|error| Self::stage_fail(submission_id, "ts-vtx-end", error))?,
                    Self::timestamp_address(
                        objects,
                        &self.vm,
                        translated.fragment_timestamps.start,
                        &mut timestamp_aliases,
                    )
                    .map_err(|error| Self::stage_fail(submission_id, "ts-frag-start", error))?,
                    Self::timestamp_address(
                        objects,
                        &self.vm,
                        translated.fragment_timestamps.end,
                        &mut timestamp_aliases,
                    )
                    .map_err(|error| Self::stage_fail(submission_id, "ts-frag-end", error))?,
                ];
                pr_info!(
                    "G17P submit: submission {} render timestamps resolved, entering manager publish\n",
                    submission_id
                );
                self.runtime
                    .mutex()
                    .lock()
                    .submit_translated_render(
                        &self.vm,
                        lease.slot(),
                        true,
                        self.async_backend
                            .as_ref()
                            .ok_or(EIO)?
                            .context
                            .clone(),
                        &translated,
                        timestamps,
                        timestamp_aliases,
                    )
                    .map_err(|error| {
                        Self::stage_fail(submission_id, "submit-translated-render", error)
                    })?;
                self.runtime
                    .wait_render_slot(lease.slot())
                    .map_err(|error| {
                        Self::stage_fail(submission_id, "wait-render-slot", error)
                    })?;
            }
            G17PPendingCommand::Compute(translated) => {
                let start = Self::timestamp_destination(objects, translated.timestamps.start)
                    .map_err(|error| Self::stage_fail(submission_id, "ts-compute-start", error))?;
                let end = Self::timestamp_destination(objects, translated.timestamps.end)
                    .map_err(|error| Self::stage_fail(submission_id, "ts-compute-end", error))?;
                let owned_timestamps = self
                    .owned_compute_timestamps()
                    .map_err(|error| {
                        Self::stage_fail(submission_id, "owned-compute-timestamps", error)
                    })?;
                // The publish runs under the runtime mutex; the wait must not.
                // The firmware reports completion through the RTKit doorbell,
                // and the worker that drains it needs this same mutex -- so
                // holding it across the wait (as the old in-submit poll did)
                // guarantees the completion can never be observed.
                pr_info!("G17P submit: compute arm, submission {}\n", submission_id);
                // The Vulkan dispatch faults on a correctly-encoded program
                // pointer (level-2 read fault), which means the page is simply
                // not resident. Report whether the two bases the driver *does*
                // know about are mapped, so an unmapped USC window shows up here
                // instead of only as a firmware fault address.
                pr_info!(
                    "G17P submit: usc_exec_base={:#x} mapped={} cs_base={:#x} mapped={} cs_end={:#x}\n",
                    translated.usc_exec_base,
                    self.vm.covers_range(translated.usc_exec_base, mmu::UAT_PGSZ as u64, false, false),
                    translated.control_stream_base,
                    self.vm.covers_range(translated.control_stream_base, mmu::UAT_PGSZ as u64, false, false),
                    translated.control_stream_end,
                );
                self.log_vm_pointer_audit(&translated);
                self.runtime
                    .mutex()
                    .lock()
                    .log_compute_status_pages("pre-doorbell");
                self.runtime
                    .mutex()
                    .lock()
                    .log_compute_sksm_entries("pre-doorbell");
                let outcome = self
                    .runtime
                    .mutex()
                    .lock()
                    .submit_translated_compute(
                        self.id,
                        &self.vm,
                        &translated,
                        owned_timestamps,
                    )
                    .map_err(|error| {
                        Self::stage_fail(submission_id, "submit-translated-compute", error)
                    })?;
                self.runtime
                    .mutex()
                    .lock()
                    .log_compute_status_pages("post-doorbell");
                let completion = match outcome {
                    g17_live_boot::G17PSubmitOutcome::Completed(completion) => completion,
                    g17_live_boot::G17PSubmitOutcome::Pending(signal) => {
                        let woken = signal.wait_timeout(G17P_COMPUTE_COMPLETION_TIMEOUT_MS);
                        match woken {
                            Some(completion) => {
                                self.runtime.mutex().lock().log_gate("post-doorbell");
                                self.runtime
                                    .mutex()
                                    .lock()
                                    .log_compute_status_pages("post-completion");
                                self.runtime
                                    .mutex()
                                    .lock()
                                    .log_compute_sksm_entries("post-completion");
                                pr_info!(
                                    "G17P submit: doorbell wait woken, timestamps=[{:#x},{:#x}]\n",
                                    completion.timestamps[0],
                                    completion.timestamps[1]
                                );
                                let finished =
                                    self.runtime.mutex().lock().finish_compute_submission();
                                match finished {
                                    Ok(completion) => completion,
                                    Err(error) => {
                                        pr_info!(
                                            "G17P submit: finish after doorbell failed ({:?})\n",
                                            error
                                        );
                                        return Err(error);
                                    }
                                }
                            }
                            None => {
                                pr_info!("G17P submit: doorbell wait TIMED OUT\n");
                                // NOTE: do NOT read the MMU fault bank here.
                                // `log_fault_bank()` touches sgx+0xd8xx, which
                                // SErrors unless the GPU cores are powered -- and
                                // by the time a submit has TIMED OUT the cores
                                // have powered back down (the pause mask returns
                                // to 0x2, "cores off"). Doing it here paniced the
                                // machine on every timeout:
                                //   readq_relaxed -> log_fault_bank -> submit
                                //   -> Kernel panic: Asynchronous SError Interrupt
                                // The address it was added to recover
                                // (0x447ffeb04) is already known, and the Vulkan
                                // fault it diagnosed is a Mesa-side bug.
                                {
                                    let mut rt = self.runtime.mutex().lock();
                                    rt.dump_completion_records("timeout");
                                    rt.log_compute_status_pages("timeout");
                                    rt.abandon_compute_submission();
                                }
                                return Err(ETIMEDOUT);
                            }
                        }
                    }
                };
                Self::copy_timestamp(start, completion[0])
                    .map_err(|error| Self::stage_fail(submission_id, "copy-ts-start", error))?;
                Self::copy_timestamp(end, completion[1])
                    .map_err(|error| Self::stage_fail(submission_id, "copy-ts-end", error))?;
            }
        }

        pr_info!("G17P submit: OK submission {}\n", submission_id);
        output_fence.signal();
        let fence = Fence::from_fence(&output_fence);
        let outputs = syncs.drain(in_sync_count..);
        Self::install_output_fences(outputs, &fence);
        dev_dbg!(
            self.dev.as_ref(),
            "G17P queue {} completed synchronous submission {} (one hardware command)\n",
            self.id,
            submission_id
        );
        Ok(())
    }
}
