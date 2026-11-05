// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! G18 (J873, T8152/G18G) DRM render-node adapter.
//!
//! A structural copy of the M5 adapter (`m5_drm.rs`): synchronous
//! submission, one hardware command per submit, out-fences signalled by a
//! publication guard. It differs where G18 differs:
//! - render commands go through `g18_application` to a job sink the G18
//!   runtime implements, not through a captured reference profile;
//! - `map_timestamp_buffer` is implemented (contract K8), so Mesa's G18G
//!   screen exposes timer queries and with them GL 3.3-4.6;
//! - compute is refused unless the adapter is built with `admit_compute`.
//!
//! The adapter holds no firmware state. Everything it needs from the live
//! G18 session goes through [`G18DrmRuntime`].
//!
//! [`asahi_gem_prime_import`] is the driver's `drm_driver.gem_prime_import`
//! hook. On the G18 device it stops foreign dma-bufs from becoming bare
//! shmem objects which asahi ioctls then misread (module 1134); every other
//! device keeps the DRM core's default import, as on t6050-gpu.

use core::ops::Range;
use core::sync::atomic::{fence, AtomicBool, AtomicPtr, Ordering};
use kernel::{bindings, c_str, macros::vtable, new_mutex, prelude::*, static_lock_class,
    dma_fence::{Fence, FenceContexts, FenceObject, FenceOps, RawDmaFence, UserFence},
    sync::{Arc, Mutex}, uapi, xarray};
use crate::{alloc, file, g18::uapi::command as g17_uapi, g18::hw,
    g18::address_map as va,
    g18::uapi::application::{self as g18_application, G18ApplicationRefusal, G18RenderJobSink, G18RenderRetirement},
    gem, gpu, g18::fw::vm_layout as m5_vm, mmu, queue};

const USER_END: u64 = (1 << 42) - 2 * mmu::UAT_PGSZ as u64;
/// Largest compute control stream admitted, in bytes. The kernel does not read
/// the stream (the CDM does); this only bounds what the firmware is pointed at.
/// 64 KiB is a sizing choice, not a hardware limit: no decode names one. It
/// admits a stream of many launches, which cost 2.10 ms per op on the M6 against
/// 10.45 ms when each op is its own stream. Only reachable with `admit_compute`.
pub(crate) const G18_COMPUTE_STREAM_BYTES: u64 = 64 * 1024;
/// `DRM_ASAHI_COMPUTE_USES_TEXTURE_STATE`: the dispatch binds texture or image
/// state, so the command takes the native texture compute mode (d62a,
/// `g18_application_compute_resources.rs`). The only compute flag G18 admits.
/// Spelled as its value, `(1 << 1)` in include/uapi/drm/asahi_drm.h, because
/// the incremental module build reuses pinned bindgen output that predates it.
const G18_COMPUTE_TEXTURE_STATE: u32 = 1 << 1;

// Identity Mesa's G18G bring-up gate accepts (`agx_chip.c:471-477`) and the
// drm-shim `g18g` profile publishes (`asahi_noop.c:129-147`), measured on
// J873: two clusters of six cores (ioreg core_mask_list=(63,63)).
const G18_CHIP_ID: u32 = 0x8152;
const G18_CLUSTERS: u32 = 2;
const G18_CORES_PER_CLUSTER: u32 = 6;
const G18_CORE_MASK: u32 = 0x3f;
const G18_USC_GENERATION: u32 = 3;
/// Revision B1, what the Linux SGX ID probe decodes on J873. Used only when
/// the runtime supplies no observed identity.
const G18_REVISION_B1: u32 = hw::GpuRevision::B1 as u32;
/// `DRM_ASAHI_GPU_HAL_300`: the UAPI has no HAL400 value yet and Mesa's
/// G18G gate requires 300. Apple's kext is PI_400.
const G18_UAPI_HAL_GENERATION: u32 = 300;

/// Kernel-VM window for K8 timestamp buffers. It must stay outside every
/// fixed G18 reservation; the assertion below re-checks that whenever the
/// address map changes.
pub(crate) const G18_TIMESTAMP_KERNEL_RANGE: Range<u64> =
    (va::KERNEL_BASE + 0xc600_0000)..(va::KERNEL_BASE + 0xc700_0000);

const fn timestamp_range_is_free() -> bool {
    let (start, end) = (G18_TIMESTAMP_KERNEL_RANGE.start, G18_TIMESTAMP_KERNEL_RANGE.end);
    let mut i = 0;
    while i < va::REGIONS.len() {
        let region = va::REGIONS[i];
        if start < region.base + region.bytes && region.base < end {
            return false;
        }
        i += 1;
    }
    true
}
const _: () = assert!(timestamp_range_is_free());

static QUEUE_NAME: &CStr = c_str!("g18_queue");
static QUEUE_CLASS_KEY: Pin<&'static kernel::sync::LockClassKey> = static_lock_class!();

/// What the adapter needs from the live G18 session. The conductor
/// implements it over the G18 runtime once firmware and the graphics
/// queues are up (see the G18 DRM adapter notes).
pub(crate) trait G18DrmRuntime: G18RenderJobSink {
    /// Any crash, fault or quarantine that ends submission for this boot.
    fn is_crashed(&self) -> bool;
    /// VM_CREATE: a user VM in the session UAT. The adapter has already
    /// checked `kernel_range` against the M5/Mesa compact contract.
    fn new_user_vm(&self, id: u64, kernel_range: Range<u64>) -> Result<mmu::Vm>;
    /// K8: map a timestamp BO range. [`map_timestamp_buffer_in`] is the
    /// intended implementation.
    fn map_timestamp_buffer(&self, bo: gem::ObjectRef, range: Range<usize>)
        -> Result<mmu::KernelMapping>;
    /// Compute (K1). Only called when the adapter admits compute.
    fn submit_compute(&self, _vm: &mmu::Vm, _command: &g17_uapi::TranslatedComputeCommand)
        -> Result<G18RenderRetirement> {
        Err(ENOTSUPP)
    }
}

/// K8 mapping for [`G18DrmRuntime::map_timestamp_buffer`]: the G17P manager
/// body (`g17_manager.rs:3949-3962`) in the G18 kernel VM, inside
/// [`G18_TIMESTAMP_KERNEL_RANGE`]. The adapter writes timestamps through the
/// mapping's CPU view; the firmware-shared GPU view is what the file layer
/// retains and what firmware-written timestamps will use later.
pub(crate) fn map_timestamp_buffer_in(uat: &mmu::Uat, mut bo: gem::ObjectRef,
    range: Range<usize>) -> Result<mmu::KernelMapping> {
    bo.map_range_into_range(uat.kernel_vm(), range, G18_TIMESTAMP_KERNEL_RANGE,
        mmu::UAT_PGSZ as u64, mmu::PROT_FW_SHARED_RW, false)
}

/// Adapter options fixed at registration.
#[derive(Debug, Copy, Clone, Default)]
pub(crate) struct G18DrmConfig {
    /// Admit compute commands (module parameter `g18_admit_compute`, default 0).
    pub(crate) admit_compute: bool,
    /// Admit exactly the native 4x MSAA render set (module parameter
    /// `g18_admit_msaa4`, default 0; msaa163). Off refuses every
    /// multisampled render as before.
    pub(crate) admit_msaa4: bool,
    /// Admit renders carrying `DRM_ASAHI_RENDER_G18_RESOURCES` (module
    /// parameter `g18_admit_graphics_scratch`, default 0). Off leaves the
    /// feature unadvertised and refuses the flag, so Mesa keeps refusing
    /// draws whose programs need a thread-private frame.
    pub(crate) admit_graphics_scratch: bool,
}

pub(crate) struct G18DrmGpu {
    runtime: Arc<dyn G18DrmRuntime>,
    /// Serializes job-sink calls across every queue of this device.
    submit_lock: Arc<Mutex<()>>,
    config: G18DrmConfig,
    revision: u32,
    ids: gpu::SequenceIDs,
}

impl G18DrmGpu {
    /// `observed` is the SGX identity read after firmware start, if the
    /// runtime has one. It must match the published topology exactly.
    pub(crate) fn new(runtime: Arc<dyn G18DrmRuntime>, observed: Option<&hw::GpuIdConfig>,
        config: G18DrmConfig) -> Result<Self> {
        let revision = match observed {
            Some(id) => {
                if id.gpu_gen != hw::GpuGen::G18 || id.gpu_variant != hw::GpuVariant::G
                    || id.num_dies != 1 || id.num_clusters != G18_CLUSTERS
                    || id.num_cores != G18_CORES_PER_CLUSTER
                    || id.usc_generation != G18_USC_GENERATION
                    || id.core_masks.as_slice() != [G18_CORE_MASK; G18_CLUSTERS as usize] {
                    pr_err!("G18_DRM: observed identity {:?} is not the J873 G18G topology\n", id);
                    return Err(ENODEV);
                }
                id.gpu_rev as u32
            }
            None => G18_REVISION_B1,
        };
        let submit_lock = Arc::pin_init(new_mutex!(()), GFP_KERNEL)?;
        Ok(Self { runtime, submit_lock, config, revision, ids: Default::default() })
    }
}

// Module 1134 reached G18 through its `DrmGpu` trait (drm_gpu.rs) and
// computed GET_PARAMS in file.rs from `DrmGpu::params`; here the same facts
// fill `drm_asahi_params_global` through `gpu::Gpu::get_params`. file.rs
// adds VM_STATUS (supports_vm_status), leaves out VM_STATUS_MIRROR
// (supports_vm_status_mirror is false, as in 1134) and INDEPENDENT_QUEUES
// (no queue limits), and adds SOFT_FAULTS when fault_control is 0xb.
impl gpu::Gpu for G18DrmGpu {
    fn as_any(&self) -> &dyn core::any::Any { self }
    fn arc_as_any(self: Arc<Self>) -> Arc<dyn core::any::Any + Sync + Send> { self }
    fn manager(&self) -> Option<&dyn gpu::GpuManager> { None }
    fn init(&self) -> Result {
        if self.is_crashed() { Err(ENODEV) } else { Ok(()) }
    }
    fn ids(&self) -> &gpu::SequenceIDs { &self.ids }
    fn is_crashed(&self) -> bool { self.runtime.is_crashed() }
    fn get_params(&self, params: &mut uapi::drm_asahi_params_global) -> Result {
        let mut core_masks = [0; uapi::DRM_ASAHI_MAX_CLUSTERS as usize];
        for mask in core_masks.get_mut(..G18_CLUSTERS as usize).ok_or(EIO)? {
            *mask = G18_CORE_MASK;
        }
        params.gpu_generation = hw::GpuGen::G18 as u32;
        params.gpu_variant = hw::GpuVariant::G as u32;
        params.gpu_revision = self.revision;
        params.chip_id = G18_CHIP_ID;
        params.num_dies = 1;
        params.num_clusters_total = G18_CLUSTERS;
        params.num_cores_per_cluster = G18_CORES_PER_CLUSTER;
        params.core_masks = core_masks.map(|mask| mask.into());
        // No G18 clock policy is implemented by this adapter.
        params.max_frequency_khz = 0;
        params.usc_generation = G18_USC_GENERATION;
        params.gpu_hal_generation = G18_UAPI_HAL_GENERATION;
        params.max_commands_per_submission = 1;
        if self.config.admit_graphics_scratch {
            params.features |= uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_G18_GRAPHICS_SCRATCH as u64;
        }
        params.features |= uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_G18_NATIVE_TILE_SAMPLES as u64;
        Ok(())
    }
    fn supports_vm_status(&self) -> bool { true }
    fn supports_vm_status_mirror(&self) -> bool { false }
    /// Module 1134 validated and resolved every VM_BIND op before committing.
    fn batched_vm_bind(&self) -> bool { true }
    /// Module 1134 recorded `submission_error()` (EIO once crashed) on every VM_STATUS query.
    fn submission_error(&self) -> Option<Error> { self.is_crashed().then_some(EIO) }
    /// Module 1134 copied the command buffer before it parsed the syncs.
    fn copy_commands_before_syncs(&self) -> bool { true }
    /// Module 1134 refused HIGH and REALTIME queues with EINVAL.
    fn allows_elevated_queue_priority(&self) -> bool { false }
    /// Module 1134 had no GEM_MADVISE ioctl.
    fn supports_gem_madvise(&self) -> bool { false }
    /// Module 1134 refused bind flag bit 4 with EINVAL, and the T8152 UAT maps no
    /// sparse tiles: a failed tile bind would clear the whole range, driver aliases
    /// included.
    fn supports_sparse_tiles(&self) -> bool { false }
    /// `DRM_IOCTL_ASAHI_GET_TIME` scales CNTPCT_EL0 by this rate
    /// (`file.rs:1225-1241`); report the counter's own frequency, as G17P
    /// does (`g17_drm.rs:1209-1222`). J873 runs it at 24 MHz.
    fn base_clock_hz(&self) -> u32 { counter_frequency() as u32 }
    /// 42-bit T8152 user space: user range UAT_PGSZ..USER_END with the dummy
    /// page at USER_END, as in module 1134.
    fn uat_geometry(&self) -> mmu::UatGeometry { mmu::UatGeometry { ias: 42, oas: 42 } }
    fn new_vm(&self, kernel_range: Range<u64>) -> Result<mmu::Vm> {
        if self.is_crashed() { return Err(ENODEV); }
        m5_vm::compact_range(&kernel_range).ok_or(EINVAL)?;
        let vm = self.runtime.new_user_vm(self.ids.vm.next(), kernel_range)?;
        // Module 1134 VM_CREATE: a simplefb alias is T8140-only; any other
        // GPU refuses the VM when the diagnostic parameter is set.
        if *crate::module_parameters::g17p_simplefb_iova.value() != 0 {
            return Err(ENOTSUPP);
        }
        Ok(vm)
    }
    fn new_queue(&self, vm: mmu::Vm,
        _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        _ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        priority: u32, usc_exec_base: u64) -> Result<KBox<dyn queue::Queue>> {
        if priority > 3 { return Err(EINVAL); }
        let window = g17_uapi::QueueUscWindow {
            base: usc_exec_base, user_start: m5_vm::DRIVER_BASE, user_end: USER_END,
        };
        window.validate().map_err(|_| EINVAL)?;
        let queue = G18Queue {
            runtime: self.runtime.clone(), submit_lock: self.submit_lock.clone(),
            config: self.config, vm, window,
            fences: FenceContexts::new(1, QUEUE_NAME, QUEUE_CLASS_KEY)?,
        };
        Ok(KBox::new(queue, GFP_KERNEL)?)
    }
    /// K8. Mesa binds `128 * sizeof(struct agx_timestamps)` of a WRITEBACK
    /// BO at screen creation and enables timer queries only if this works.
    fn map_timestamp_buffer(&self, bo: gem::ObjectRef, range: Range<usize>)
        -> Result<mmu::KernelMapping> {
        if self.is_crashed() { return Err(ENODEV); }
        if range.is_empty() { return Err(EINVAL); }
        self.runtime.map_timestamp_buffer(bo, range)
    }
}

/// CNTFRQ_EL0, or Apple's 24 MHz if boot firmware left it unprogrammed:
/// GET_TIME divides by this rate (`file.rs:1225-1229`) and must never see 0.
fn counter_frequency() -> u64 {
    let frequency: u64;
    // SAFETY: CNTFRQ_EL0 is a read-only system register.
    unsafe { core::arch::asm!("mrs {x}, CNTFRQ_EL0", x = out(reg) frequency) };
    if frequency == 0 { 24_000_000 } else { frequency }
}

/// The counter `DRM_IOCTL_ASAHI_GET_TIME` reads, ordered after prior code.
fn counter_ticks() -> u64 {
    let ticks: u64;
    // SAFETY: the ISB only orders the read; CNTPCT_EL0 is read-only.
    unsafe { core::arch::asm!("isb", "mrs {x}, CNTPCT_EL0", x = out(reg) ticks) };
    ticks
}

/// GET_TIME's conversion (`file.rs:1225-1241`): floor(ticks * 1e9 / f), the
/// same rational as its gcd-reduced form. Split at whole seconds so no step
/// overflows (the remainder term is below f * 1e9 < 2^64 for any u32 rate)
/// and no 128-bit division (`__udivti3`) reaches the module.
fn ticks_to_ns(ticks: u64, frequency: u64) -> u64 {
    const NSEC_PER_SEC: u64 = 1_000_000_000;
    (ticks / frequency).wrapping_mul(NSEC_PER_SEC)
        .wrapping_add(ticks % frequency * NSEC_PER_SEC / frequency)
}

struct G18SubmissionFence;
#[vtable]
impl FenceOps for G18SubmissionFence {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("asahi") }
    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("g18-submit") }
}

/// Keep the original sync vector: creating a second output list would add an
/// allocation failure after userspace has already supplied its wait targets.
struct OutputPublication {
    fence: UserFence<G18SubmissionFence>,
    syncs: KVec<file::SyncItem>,
    input_count: usize,
    result: Result,
}

impl Drop for OutputPublication {
    fn drop(&mut self) {
        if let Err(error) = self.result { self.fence.set_error(error); }
        let fence = Fence::from_fence(&self.fence);
        for mut sync in self.syncs.drain(self.input_count..) {
            if let Some(chain) = sync.chain_fence.take() {
                sync.syncobj.add_point(chain, &fence, sync.timeline_value);
            } else { sync.syncobj.replace_fence(Some(&fence)); }
        }
        self.fence.signal();
    }
}

struct UserAddressSpace<'a> { vm: &'a mmu::Vm }
impl g17_uapi::GpuAddressSpace for UserAddressSpace<'_> {
    fn covers(&self, address: u64, size: u64, access: g17_uapi::GpuAccess) -> bool {
        let (read, write) = match access {
            g17_uapi::GpuAccess::Read => (true, false),
            g17_uapi::GpuAccess::Write => (false, true),
            g17_uapi::GpuAccess::ReadWrite => (true, true),
        };
        m5_vm::user_range_contains(&self.vm.kernel_range(), address, size, USER_END)
            && self.vm.covers_range(address, size, read, write)
    }
}

/// One UAPI timestamp destination inside a bound timestamp buffer.
struct TimestampSlot {
    mapping: Arc<mmu::KernelMapping>,
    offset: usize,
}

impl TimestampSlot {
    /// Resolve before any GPU work, so a bad handle fails the submit cleanly.
    fn resolve(objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: g17_uapi::UapiTimestamp) -> Result<Option<Self>> {
        if timestamp.handle == 0 {
            return if timestamp.offset == 0 { Ok(None) } else { Err(EINVAL) };
        }
        let guard = objects.lock();
        let object = guard.get(timestamp.handle.try_into()?).ok_or(ENOENT)?.clone();
        core::mem::drop(guard);
        match object {
            file::Object::TimestampBuffer(mapping) => {
                let offset: usize = timestamp.offset.try_into()?;
                if offset.checked_add(8).ok_or(EINVAL)? > mapping.size() {
                    return Err(ERANGE);
                }
                Ok(Some(Self { mapping, offset }))
            }
        }
    }

    /// Publish one timestamp in the UAPI unit, GET_TIME nanoseconds.
    fn write(&self, ticks: u64, frequency: u64) -> Result {
        let value = ticks_to_ns(ticks, frequency);
        self.mapping.with_cpu_bytes(|bytes| {
            let end = self.offset.checked_add(8).ok_or(EINVAL)?;
            bytes.get_mut(self.offset..end).ok_or(EINVAL)?
                .copy_from_slice(&value.to_le_bytes());
            Ok(())
        }).inspect_err(|error| pr_err!(
            "G18 timestamp not published: {:?} offset={:#x} mapping={:#x}\n",
            error, self.offset, self.mapping.size()))?;
        fence(Ordering::Release);
        Ok(())
    }
}

/// Write the requested stage timestamps after retirement. Without job stage
/// ticks, the CPU bracket `[t0, t1]` around the synchronous job bounds every
/// stage: starts get t0, ends get t1.
fn publish_timestamps(slots: &[Option<TimestampSlot>], stage_ticks: &[u64]) -> Result {
    let frequency = counter_frequency();
    for (slot, &ticks) in slots.iter().zip(stage_ticks) {
        if let Some(slot) = slot { slot.write(ticks, frequency)?; }
    }
    Ok(())
}

struct G18Queue {
    runtime: Arc<dyn G18DrmRuntime>,
    submit_lock: Arc<Mutex<()>>,
    config: G18DrmConfig,
    vm: mmu::Vm,
    window: g17_uapi::QueueUscWindow,
    fences: FenceContexts,
}

impl queue::Queue for G18Queue {
    fn submit(&mut self, _submission_id: u64, syncs: KVec<file::SyncItem>,
        in_sync_count: usize, cmdbuf_raw: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result {
        if in_sync_count > syncs.len() { return Err(EINVAL); }
        let fence = self.fences.new_fence(0, G18SubmissionFence)?.into();
        let mut publication = OutputPublication {
            fence, syncs, input_count: in_sync_count, result: Err(ECANCELED),
        };
        let result = self.submit_one(&publication.syncs[..in_sync_count], cmdbuf_raw, objects);
        // The runtime records and quarantines a failed published VM before
        // this output fence is installed/signalled by the publication guard.
        publication.result = result;
        result
    }
}

impl G18Queue {
    fn submit_one(&self, inputs: &[file::SyncItem], cmdbuf_raw: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result {
        if self.vm.status().map_or(0, |s| s.get()) != 0 { return Err(EIO); }
        if self.runtime.is_crashed() { return Err(ENODEV); }
        // Wait outside the submission lock; another client may own the job
        // whose fence satisfies this submission's dependency.
        for sync in inputs {
            if let Some(fence) = &sync.fence {
                // SAFETY: both calls borrow the strongly held input fence.
                let waited = unsafe { bindings::dma_fence_wait_timeout(
                    fence.raw(), true, kernel::task::MAX_SCHEDULE_TIMEOUT) };
                if waited < 0 { return Err(Error::from_errno(waited as i32)); }
                if waited == 0 { return Err(ETIMEDOUT); }
                let status = unsafe { bindings::dma_fence_get_status(fence.raw()) };
                if status < 0 { return Err(Error::from_errno(status)); }
            }
        }
        // An empty command buffer is a synchronization-only submission. Input
        // fences have completed above; the normal publication guard installs
        // and signals its output fence, preserving timeline chaining. Never
        // manufacture a hardware job or relax hardware retirement checks.
        if cmdbuf_raw.is_empty() {
            let _serialized = self.submit_lock.lock();
            if self.vm.status().map_or(0, |s| s.get()) != 0 { return Err(EIO); }
            if self.runtime.is_crashed() { return Err(ENODEV); }
            pr_info!("G18_SYNC_ONLY inputs={}\n", inputs.len());
            return Ok(());
        }
        let _vm_job = self.vm.retain_t8140_job()?;
        // Parsing and translation frames end here, before the job runs its
        // own register builders on this stack.
        let admitted = self.admit(cmdbuf_raw, objects)?;
        let _serialized = self.submit_lock.lock();
        // Commands run in submission order and each retires before the next
        // starts, so every barrier (an earlier command of this submission, or
        // any earlier submission) has completed when its waiter begins.
        for command in admitted {
            if self.runtime.is_crashed() { return Err(ENODEV); }
            let t0 = counter_ticks();
            match command {
                Admitted::Render(render, slots) => {
                    let retired = self.runtime.submit_render(&self.vm, &render)?;
                    let t1 = counter_ticks();
                    publish_timestamps(&slots, &retired.stage_ticks.unwrap_or([t0, t1, t0, t1]))?;
                }
                Admitted::Compute(command, slots) => {
                    let retired = self.runtime.submit_compute(&self.vm, &command)?;
                    let t1 = counter_ticks();
                    let [start, _, _, end] = retired.stage_ticks.unwrap_or([t0, t1, t0, t1]);
                    publish_timestamps(&slots, &[start, end])?;
                }
            }
        }
        Ok(())
    }

    /// Parse every hardware command of a submission, apply the G18 admission
    /// rules and resolve their timestamp destinations, all before any runs:
    /// a submission is admitted whole or not at all. Nothing here touches
    /// the GPU. A compute prepass and the render that consumes it (transform
    /// feedback, geometry and tessellation draws) arrive as one submission.
    #[inline(never)]
    fn admit(&self, cmdbuf_raw: &[u8], objects: Pin<&xarray::XArray<KBox<file::Object>>>)
        -> Result<KVec<Admitted>> {
        let malformed = |error: g17_uapi::UapiTranslateError| {
            pr_err!("G18 submit admission rejected: {:?}\n", error);
            EINVAL
        };
        let mut parser = g17_uapi::UapiCommandParser::new(cmdbuf_raw);
        let mut admitted = KVec::new();
        while let Some(command) = parser.next_hardware().map_err(malformed)? {
            let command = match command {
                g17_uapi::ParsedHardwareCommand::Render {
                    payload, vertex_attachments, fragment_attachments, ..
                } => self.admit_render(&payload, vertex_attachments, fragment_attachments, objects)?,
                g17_uapi::ParsedHardwareCommand::Compute { payload, attachments, .. } =>
                    self.admit_compute(&payload, attachments, objects)?,
            };
            admitted.push(command, GFP_KERNEL)?;
        }
        parser.finish().map_err(malformed)?;
        Ok(admitted)
    }

    #[inline(never)]
    fn admit_render(&self, payload: &g17_uapi::UapiRenderCommand,
        vertex_attachments: g17_uapi::UapiAttachmentList,
        fragment_attachments: g17_uapi::UapiAttachmentList,
        objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result<Admitted> {
        let address_space = UserAddressSpace { vm: &self.vm };
        let scratch = if payload.flags & g17_uapi::RENDER_G18_RESOURCES != 0 {
            Some(self.admit_render_scratch(payload)?)
        } else {
            None
        };
        // The shared translator refuses the G18 flag and never sees the
        // suffix. Only this adapter consumes and strips a validated suffix.
        let mut untagged = *payload;
        untagged.flags &= !g17_uapi::RENDER_G18_RESOURCES;
        untagged.g18_resources = [0; 20];
        let mut render = g18_application::translate_g18_render_admitting(&untagged,
            vertex_attachments, fragment_attachments, self.window, &address_space,
            self.config.admit_msaa4)
            .map_err(|refusal| refuse_render(payload, refusal))?;
        render.scratch = scratch;
        let render = KBox::new(render, GFP_KERNEL)?;
        if !g18_application::job_owned_fields_clear(&render.parameters, &render.controls) {
            pr_err!("G18 render admission rejected: job-owned fields set\n");
            return Err(EINVAL);
        }
        let slots = [
            TimestampSlot::resolve(objects, payload.vertex_timestamps.start)?,
            TimestampSlot::resolve(objects, payload.vertex_timestamps.end)?,
            TimestampSlot::resolve(objects, payload.fragment_timestamps.start)?,
            TimestampSlot::resolve(objects, payload.fragment_timestamps.end)?,
        ];
        Ok(Admitted::Render(render, slots))
    }

    /// Derive the TA/3D private-memory selection for a flagged render. Only
    /// single-sample, single-layer renders with one-page tile storage are in
    /// the evidenced envelope (g18_graphics_scratch.rs). The selection never
    /// resizes the owned pool: ideal when both kicks fit it, else the
    /// native minimum result, else ENOSPC. Nothing is published on refusal.
    #[inline(never)]
    fn admit_render_scratch(&self, payload: &g17_uapi::UapiRenderCommand)
        -> Result<crate::g18::uapi::graphics_scratch::G18RenderScratchSelection> {
        use crate::{g18::uapi::graphics_scratch::G18RenderMetadata, g18::uapi::render_policy as policy};
        if !self.config.admit_graphics_scratch {
            pr_err!("G18 render admission rejected: graphics scratch is disabled (g18_admit_graphics_scratch=0)\n");
            return Err(ENOTSUPP);
        }
        if payload.samples != 1 || payload.layers != 1
            || (payload.utile_width, payload.utile_height) != (32, 32) {
            pr_err!("G18 render admission rejected: graphics scratch outside single-sample 32x32 envelope samples={} layers={} utiles={}x{}\n",
                payload.samples, payload.layers, payload.utile_width, payload.utile_height);
            return Err(ENOTSUPP);
        }
        let tile_bytes = 32 * 32 * u32::from(payload.sample_size);
        let metadata = G18RenderMetadata::decode(&payload.g18_resources, tile_bytes)
            .ok_or_else(|| {
                pr_err!("G18 render admission rejected: invalid graphics scratch metadata words={:x?} tile_bytes={}\n",
                    payload.g18_resources, tile_bytes);
                EINVAL
            })?;
        let plan = metadata.plan(policy::RENDER_POOL_BLOCK_BYTES).ok_or_else(|| {
            pr_err!("G18 render admission rejected: graphics scratch exceeds the native pool capacity\n");
            ENOSPC
        })?;
        let (owned_bytes, owned_chunks) = policy::owned_scratch_capacity();
        let selected = plan.select(owned_bytes, owned_chunks).ok_or_else(|| {
            pr_err!("G18 render admission rejected: graphics scratch needs {:?} beyond owned pool {} bytes / {} chunks\n",
                plan.minimum, owned_bytes, owned_chunks);
            ENOSPC
        })?;
        pr_info!("G18_RENDER_SCRATCH vertex={:?} fragment={:?} tiles={:?} tile_bytes={} mode={:?} ta={:?} d3={:?}\n",
            metadata.vertex, metadata.fragment, metadata.tiles, tile_bytes, selected.mode,
            selected.stages[0], selected.stages[1]);
        Ok(selected)
    }

    #[inline(never)]
    fn admit_compute(&self, payload: &g17_uapi::UapiComputeCommand,
        attachments: g17_uapi::UapiAttachmentList,
        objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result<Admitted> {
        if !self.config.admit_compute {
            pr_err!("G18 compute admission rejected: compute is disabled (g18_admit_compute=0)\n");
            return Err(ENOTSUPP);
        }
        let stream_bytes = payload.control_stream_end.saturating_sub(payload.control_stream_base);
        let g18_flags = G18_COMPUTE_TEXTURE_STATE | g17_uapi::COMPUTE_G18_RESOURCES;
        // ADD3 and every flag outside the G18 contract remain refused.
        if payload.flags & !g18_flags != 0 || payload.sampler_heap != 0
            || payload.sampler_count != 0 || stream_bytes > G18_COMPUTE_STREAM_BYTES {
            pr_err!("G18 compute admission rejected flags={:#x} sampler_heap={:#x} samplers={} stream_bytes={} (admits at most {} bytes)\n",
                payload.flags, payload.sampler_heap, payload.sampler_count, stream_bytes,
                G18_COMPUTE_STREAM_BYTES);
            return Err(ENOTSUPP);
        }
        let resource_descriptor = if payload.flags & g17_uapi::COMPUTE_G18_RESOURCES != 0 {
            let metadata = crate::g18::uapi::compute_metadata::G18ComputeMetadata::decode(payload.g18_resources)
                .ok_or_else(|| {
                    pr_err!("G18 compute admission rejected: invalid resource metadata\n");
                    EINVAL
                })?;
            if crate::g18::uapi::compute_plan::G18ComputePlan::from_descriptor(&metadata.descriptor).is_none() {
                pr_err!("G18 compute admission rejected: resource plan exceeds supported capacity\n");
                return Err(ENOSPC);
            }
            // Main static TPS uses the metadata-derived owned pool and SMA
            // launch pair. Mesa's G18 compute encoder carries the same aligned
            // frame in both SPECTPR scratch fields. Constant-program scratch
            // and SCS still need their separate launch contract.
            if metadata.demand.constant_scratch_bytes != 0 || payload.scs_layout != 0
            {
                pr_err!("G18 compute admission rejected: constant scratch or SCS integration pending\n");
                return Err(ENOTSUPP);
            }
            crate::g18_kick_pr_info!("G18_COMPUTE_METADATA stream={:#x} registers={} scratch_bytes={} spectpr_bytes={} constant_scratch={} local={:?}\n",
                payload.control_stream_base, payload.g18_resources[2],
                metadata.demand.scratch_bytes, payload.g18_resources[4],
                metadata.demand.constant_scratch_bytes, &payload.g18_resources[12..15]);
            Some(metadata.descriptor)
        } else {
            None
        };
        // The shared G17 translator refuses G18 flags and nonzero resource
        // words. Only this adapter may consume and strip a validated suffix.
        let mut untagged = *payload;
        untagged.flags &= !g18_flags;
        if resource_descriptor.is_some() { untagged.g18_resources = [0; 16]; }
        let address_space = UserAddressSpace { vm: &self.vm };
        let mut translated = g17_uapi::translate_compute_command(untagged, attachments,
            self.window, &address_space).map_err(|error| {
                pr_err!("G18 compute admission rejected {:?}: helper={:#x}/{:#x}/{:#x} flags={:#x}\n",
                    error, payload.helper.binary, payload.helper.config, payload.helper.data, payload.flags);
                EINVAL
            })?;
        translated.g18_texture_state = payload.flags & G18_COMPUTE_TEXTURE_STATE != 0;
        translated.g18_resource_descriptor = resource_descriptor;
        let slots = [
            TimestampSlot::resolve(objects, payload.timestamps.start)?,
            TimestampSlot::resolve(objects, payload.timestamps.end)?,
        ];
        Ok(Admitted::Compute(KBox::new(translated, GFP_KERNEL)?, slots))
    }
}

/// An admitted command, heap-held so the submit frame stays small.
enum Admitted {
    Render(KBox<g18_application::G18ApplicationRender>, [Option<TimestampSlot>; 4]),
    Compute(KBox<g17_uapi::TranslatedComputeCommand>, [Option<TimestampSlot>; 2]),
}

/// Log a render refusal with the fields that decided it and map it to the
/// M5 errno convention: EINVAL for a malformed command, ENOTSUPP otherwise.
fn refuse_render(payload: &g17_uapi::UapiRenderCommand, refusal: G18ApplicationRefusal) -> Error {
    pr_err!("G18 render admission rejected {:?}: size={}x{} layers={} utiles={}x{} samples={} msaa={:#x} flags={:#x} zls={:#x} depth={:#x}/{:#x} stencil={:#x} bgobjvals={:#x} samplers={}\n",
        refusal, payload.width, payload.height, payload.layers, payload.utile_width,
        payload.utile_height, payload.samples, payload.multisample_control, payload.flags,
        payload.zls_control, payload.depth.base, payload.depth.compression_base,
        payload.stencil.base, payload.stencil_clear, payload.sampler_count);
    if refusal.is_invalid() { EINVAL } else { ENOTSUPP }
}

static FOREIGN_IMPORT_REPORTED: AtomicBool = AtomicBool::new(false);

/// Parent `struct device` of the G18 DRM device, set before it registers.
static G18_PARENT: AtomicPtr<bindings::device> = AtomicPtr::new(core::ptr::null_mut());

/// Record the platform device of the G18 DRM device, so that
/// [`asahi_gem_prime_import`] applies module 1134's import rule to it alone.
pub(crate) fn note_g18_parent(parent: *mut bindings::device) {
    G18_PARENT.store(parent, Ordering::Release);
}

/// `drm_driver.gem_prime_import` for asahi. A device other than G18 gets the
/// DRM core's default import, exactly what the core does without this hook
/// (t6050-gpu has none). The G18 device re-imports only dma-bufs that it
/// exported. Anything else is refused with EINVAL, the DRM
/// core's answer for a driver without import support
/// (`drm_prime.c:987-988`).
///
/// Without this hook the core's default import builds a bare C
/// `drm_gem_shmem_object` (no `gem_create_object`, `shmem.rs:335-339`),
/// and every asahi ioctl that looks a handle up reads it as an
/// `Object<AsahiObject>` (`file.rs:989`, `shmem.rs:320-327`).
///
/// # Safety
///
/// The DRM core calls this with a valid `drm_device` and a dma-buf it holds
/// a reference on, for the duration of the call.
pub(crate) unsafe extern "C" fn asahi_gem_prime_import(
    dev: *mut bindings::drm_device,
    dma_buf: *mut bindings::dma_buf,
) -> *mut bindings::drm_gem_object {
    // SAFETY: `dev` is a valid drm_device (caller contract); its parent is
    // set at allocation and does not change while the device exists.
    let parent = unsafe { (*dev).dev };
    if parent.is_null() || parent != G18_PARENT.load(Ordering::Acquire) {
        // SAFETY: caller contract; this is the call the DRM core makes
        // itself when a driver has no gem_prime_import hook.
        return unsafe { bindings::drm_gem_prime_import(dev, dma_buf) };
    }
    // SAFETY: both pointers are valid (caller contract). The helper compares
    // `dma_buf->ops` with DRM's own PRIME ops before it reads `priv` as a GEM
    // object (`drm_prime.c:945-951`).
    if unsafe { bindings::drm_gem_is_prime_exported_dma_buf(dev, dma_buf) } {
        // SAFETY: for a dma-buf this device exported, the helper only takes
        // a reference on the existing GEM object (`drm_prime.c:977-983`),
        // which is an `Object<AsahiObject>` created by this driver.
        return unsafe { bindings::drm_gem_prime_import(dev, dma_buf) };
    }
    if !FOREIGN_IMPORT_REPORTED.swap(true, Ordering::Relaxed) {
        pr_warn!("asahi: refusing a dma-buf another driver exported; only asahi's own buffers can be imported\n");
    }
    EINVAL.to_ptr()
}
