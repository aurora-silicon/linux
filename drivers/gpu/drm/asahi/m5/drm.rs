// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Experimental M5 DRM adapter using the retained M5 firmware session.
//! First-job admission is deliberately separate from general Mesa support.

use core::{any::Any, ops::Range};
use kernel::{bindings, c_str, device, macros::vtable, new_mutex, prelude::*, static_lock_class,
    dma_fence::{Fence, FenceContexts, FenceObject, FenceOps, RawDmaFence, UserFence},
    sync::{Arc, Mutex}, uapi, xarray};
use crate::{alloc, driver::AsahiRegData, file, m5::live_boot::M5Runtime,
    m5::resources::M5FirmwareMemory, m5::command, gem, gpu, hw, m5::vm_layout, mmu, queue};

pub(crate) type SharedM5Runtime = Arc<Mutex<M5Runtime>>;
const USER_END: u64 = (1 << 42) - 2 * mmu::UAT_PGSZ as u64;

/// Bytes of CDM control stream the witnessed M5 compute profile covers. The
/// captured stream (compute_program::STREAM) is twelve words: one dispatch
/// and the terminator at +44. Nothing longer has native evidence, and a
/// longer one is what a geometry-shader prepass submits.
const M5_COMPUTE_STREAM_BYTES: u64 = 48;
static QUEUE_NAME: &CStr = c_str!("m5_queue");
static QUEUE_CLASS_KEY: Pin<&'static kernel::sync::LockClassKey> = static_lock_class!();

#[cfg(CONFIG_APPLE_CLPC)]
extern "C" {
    fn apple_clpc_gpu_get(dev: *mut core::ffi::c_void, out: *mut *mut core::ffi::c_void)
        -> core::ffi::c_int;
    fn apple_clpc_gpu_set_active(gpu: *mut core::ffi::c_void, active: bool);
    fn apple_clpc_gpu_put(gpu: *mut core::ffi::c_void);
}

/// The CLPC's GPU input (`apple,clpc` on the GPU node). While the PMP runs,
/// its vote leaves the DRAM controller and the fabric at their lowest states;
/// the CLPC holds them at the top of its maps while M5 work is in flight,
/// and releases them its hold time after the last of it retires. Without a
/// CLPC the GPU runs as before, only slower under the PMP.
#[pin_data(PinnedDrop)]
pub(crate) struct M5ClpcInput {
    gpu: *mut core::ffi::c_void,
    /// Submissions in flight; the input is active while it is not zero.
    #[pin]
    in_flight: Mutex<u32>,
}

// SAFETY: the CLPC serializes its input under its own lock, and the counter
// is only changed under `in_flight`.
unsafe impl Send for M5ClpcInput {}
// SAFETY: as above; shared use never changes the reference itself.
unsafe impl Sync for M5ClpcInput {}

impl M5ClpcInput {
    /// Takes the input before the firmware session starts, so that a CLPC
    /// which has not probed yet defers the GPU without stranding a session.
    pub(crate) fn new(dev: &device::Device) -> Result<Arc<Self>> {
        #[allow(unused_mut)]
        let mut gpu: *mut core::ffi::c_void = core::ptr::null_mut();
        #[cfg(CONFIG_APPLE_CLPC)]
        {
            // SAFETY: a live device and a writable result; C returns a
            // reference, or NULL when the node names no CLPC.
            let ret = unsafe { apple_clpc_gpu_get(dev.as_raw().cast(), &mut gpu) };
            if ret == EPROBE_DEFER.to_errno() {
                return Err(EPROBE_DEFER);
            }
            if ret < 0 {
                dev_warn!(dev, "no CLPC input ({}): GPU work gets no memory floor\n", ret);
                gpu = core::ptr::null_mut();
            }
        }
        #[cfg(not(CONFIG_APPLE_CLPC))]
        let _ = dev;
        Arc::pin_init(pin_init!(Self { gpu, in_flight <- new_mutex!(0) }), GFP_KERNEL)
    }

    fn set_active(&self, _active: bool) {
        #[cfg(CONFIG_APPLE_CLPC)]
        // SAFETY: NULL is accepted; a non-NULL reference is held until drop.
        unsafe { apple_clpc_gpu_set_active(self.gpu, _active) };
    }

    /// Marks one submission in flight until the returned guard drops.
    fn busy(&self) -> M5ClpcBusy<'_> {
        let mut count = self.in_flight.lock();
        *count += 1;
        if *count == 1 {
            self.set_active(true);
        }
        M5ClpcBusy(self)
    }
}

#[pinned_drop]
impl PinnedDrop for M5ClpcInput {
    fn drop(self: Pin<&mut Self>) {
        #[cfg(CONFIG_APPLE_CLPC)]
        // SAFETY: releases exactly the reference taken in new(), clearing the
        // input first; NULL is accepted.
        unsafe { apple_clpc_gpu_put(self.gpu) };
    }
}

struct M5ClpcBusy<'a>(&'a M5ClpcInput);

impl Drop for M5ClpcBusy<'_> {
    fn drop(&mut self) {
        let mut count = self.0.in_flight.lock();
        *count -= 1;
        if *count == 0 {
            self.0.set_active(false);
        }
    }
}

pub(crate) struct M5DrmGpu {
    // A strong reference prevents open DRM files from outliving the session.
    runtime: SharedM5Runtime,
    id: crate::regs::G17Id,
    ids: gpu::SequenceIDs,
    clpc: Arc<M5ClpcInput>,
}

impl M5DrmGpu {
    pub(crate) fn new(runtime: SharedM5Runtime, id: crate::regs::G17Id,
        clpc: Arc<M5ClpcInput>) -> Self {
        Self { runtime, id, ids: Default::default(), clpc }
    }
}

impl gpu::Gpu for M5DrmGpu {
    fn stats_snapshot(&self) -> Option<Arc<crate::stats::StatsSnapshot>> { None }
    fn as_any(&self) -> &dyn Any { self }
    fn arc_as_any(self: Arc<Self>) -> Arc<dyn Any + Sync + Send> { self as Arc<dyn Any + Sync + Send> }
    fn manager(&self) -> Option<&dyn gpu::GpuManager> { None }
    fn init(&self) -> Result {
        if self.is_crashed() { Err(ENODEV) } else { Ok(()) }
    }
    fn ids(&self) -> &gpu::SequenceIDs { &self.ids }
    fn is_crashed(&self) -> bool { self.runtime.lock().is_crashed() }
    fn get_params(&self, params: &mut uapi::drm_asahi_params_global) -> Result {
        params.gpu_generation = hw::GpuGen::G17 as u32;
        params.gpu_variant = hw::GpuVariant::G as u32;
        params.gpu_revision = self.id.gpu_rev as u32;
        params.chip_id = 0x8142;
        params.num_dies = self.id.num_dies;
        params.num_clusters_total = self.id.num_clusters;
        params.num_cores_per_cluster = self.id.num_cores;
        for (i, mask) in self.id.core_masks.iter().enumerate() {
            *params.core_masks.get_mut(i).ok_or(EIO)? = (*mask).into();
        }
        // Dynamic M5 clock policy is not implemented.
        params.max_frequency_khz = 0;
        params.usc_generation = 3;
        params.gpu_hal_generation = 300;
        // One hardware command per submission.
        params.max_commands_per_submission = 1;
        Ok(())
    }
    fn supports_vm_status(&self) -> bool { true }
    /// VM_BIND defers unmaps while accepted work runs, and a submission installs its fence
    /// in every output sync, also on failure.
    fn tracked_submission(&self) -> bool { true }
    fn user_range(&self) -> Range<u64> { mmu::UAT_PGSZ as u64..USER_END }
    fn base_clock_hz(&self) -> u32 { 24_000_000 }
    fn uat_geometry(&self) -> mmu::UatGeometry { mmu::UatGeometry { ias: 42, oas: 42 } }
    fn new_vm(&self, kernel_range: Range<u64>) -> Result<mmu::Vm> {
        self.runtime.lock().new_user_vm(self.ids.vm.next(), kernel_range)
    }
    fn new_queue(&self, vm: mmu::Vm,
        _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        _ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        priority: u32, usc_exec_base: u64,
        _usage: Arc<crate::usage::Client>) -> Result<KBox<dyn queue::Queue>> {
        if priority > 3 { return Err(EINVAL); }
        let window = command::QueueUscWindow {
            base: usc_exec_base, user_start: vm_layout::DRIVER_BASE, user_end: USER_END,
        };
        window.validate().map_err(|_| EINVAL)?;
        let queue = M5Queue {
            runtime: self.runtime.clone(), vm, window,
            fences: FenceContexts::new(1, QUEUE_NAME, QUEUE_CLASS_KEY)?,
            clpc: self.clpc.clone(),
        };
        Ok(KBox::new(queue, GFP_KERNEL)?)
    }
    fn map_timestamp_buffer(&self, _bo: gem::ObjectRef, _range: Range<usize>)
        -> Result<crate::timestamp::Buffer> { Err(ENOTSUPP) }
}

struct M5SubmissionFence;
#[vtable]
impl FenceOps for M5SubmissionFence {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("asahi") }
    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("m5-submit") }
}

/// Keep the original sync vector: creating a second output list would add an
/// allocation failure after userspace has already supplied its wait targets.
struct OutputPublication {
    fence: UserFence<M5SubmissionFence>,
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
impl command::GpuAddressSpace for UserAddressSpace<'_> {
    fn covers(&self, address: u64, size: u64, access: command::GpuAccess) -> bool {
        let (read, write) = match access {
            command::GpuAccess::Read => (true, false),
            command::GpuAccess::Write => (false, true),
            command::GpuAccess::ReadWrite => (true, true),
        };
        vm_layout::user_range_contains(&self.vm.kernel_range(), address, size, USER_END)
            && self.vm.covers_range(address, size, read, write)
    }
}

struct M5Queue {
    runtime: SharedM5Runtime,
    vm: mmu::Vm,
    window: command::QueueUscWindow,
    fences: FenceContexts,
    clpc: Arc<M5ClpcInput>,
}

impl queue::Queue for M5Queue {
    fn submit(&mut self, _reg_data: &AsahiRegData<'_>, _submission_id: u64,
        syncs: KVec<file::SyncItem>,
        in_sync_count: usize, cmdbuf_raw: &[u8],
        _objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result {
        if in_sync_count > syncs.len() { return Err(EINVAL); }
        let fence = self.fences.new_fence(0, M5SubmissionFence)?.into();
        let mut publication = OutputPublication {
            fence, syncs, input_count: in_sync_count, result: Err(ECANCELED),
        };
        let result = (|| {
            if self.vm.status().is_some_and(|status| status.get() != 0) { return Err(EIO); }
            // Wait outside the runtime lock; another client may own the job
            // whose fence satisfies this submission's dependency.
            for sync in &publication.syncs[..in_sync_count] {
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
            let _vm_job = self.vm.retain_job()?;
            let address_space = UserAddressSpace { vm: &self.vm };
            let mut parser = command::UapiCommandParser::new(cmdbuf_raw);
            let command = parser.next_hardware().map_err(|_| EINVAL)?.ok_or(EINVAL)?;
            if parser.next_hardware().map_err(|_| EINVAL)?.is_some() {
                pr_err!("M5 submit admission rejected: more than one hardware command\n");
                return Err(ENOTSUPP);
            }
            parser.finish().map_err(|_| EINVAL)?;
            if let command::ParsedHardwareCommand::Compute { payload, attachments, .. } = command {
                let timestamps = [payload.timestamps.start, payload.timestamps.end];
                if payload.flags != 0
                    || timestamps.iter().any(|t| t.handle != 0 || t.offset != 0) {
                    pr_err!("M5 compute admission rejected flags={:#x} sampler_heap={:#x} samplers={} timestamps={}\n",
                        payload.flags, payload.sampler_heap, payload.sampler_count,
                        timestamps.iter().any(|t| t.handle != 0 || t.offset != 0));
                    return Err(ENOTSUPP);
                }
                // The witnessed M5 compute profile is one dispatch: the
                // captured control stream is twelve words, a single dispatch
                // followed by the terminator at +44, and the descriptor
                // carries exactly one dispatch identity, one submission index
                // and one pair of dispatch stamps. A longer stream -- every
                // geometry-shader and transform-feedback prepass emits four or
                // five dispatches -- is accepted by firmware, linked into the
                // job list and never retired, so the submitting ioctl times
                // out after forty seconds and quarantines the VM. Refuse it
                // here instead: the client drops the batch and the session
                // survives, exactly as it does for an out-of-profile render.
                let stream_bytes = payload.control_stream_end
                    .saturating_sub(payload.control_stream_base);
                if stream_bytes > M5_COMPUTE_STREAM_BYTES {
                    pr_err!("M5 compute admission rejected stream_bytes={} (profile admits one dispatch, {})\n",
                        stream_bytes, M5_COMPUTE_STREAM_BYTES);
                    return Err(ENOTSUPP);
                }
                let translated = command::translate_compute_command(payload, attachments,
                    self.window, &address_space).map_err(|_| EINVAL)?;
                let _busy = self.clpc.busy();
                return self.runtime.lock().submit_application_compute(&self.vm, &translated);
            }
            let command::ParsedHardwareCommand::Render {
                payload, vertex_attachments, fragment_attachments, ..
            } = command else {
                pr_err!("M5 submit admission rejected: neither a render nor a compute command\n");
                return Err(ENOTSUPP);
            };
            admit_reference_render(&payload).map_err(|error| {
                pr_err!("M5 render admission rejected size={}x{} layers={} utiles={}x{} samples={} sample_bytes={} positions={:#x}\n",
                    payload.width, payload.height, payload.layers,
                    payload.utile_width, payload.utile_height, payload.samples,
                    payload.sample_size, payload.multisample_control);
                error
            })?;
            command::validate_m5_reference_depth(&payload, &address_space)
                .map_err(|error| {
                    pr_err!("M5 depth admission rejected {:?}: size={}x{} dimensions={:#x} control={:#x} stride={:#x} meta_stride={:#x} base={:#x} meta={:#x} clear={:#x}\n",
                        error, payload.width, payload.height, payload.depth_dimensions,
                        payload.zls_control, payload.depth.stride, payload.depth.compression_stride,
                        payload.depth.base, payload.depth.compression_base, payload.depth_clear);
                    ENOTSUPP
                })?;
            let (parameters, mut controls) = M5FirmwareMemory::reference_render_profile();
            let mut translated = command::translate_render_command(payload,
                vertex_attachments, fragment_attachments, self.window, &address_space,
                command::RenderInternalState { parameters }).map_err(|_| EINVAL)?;
            // Native single-layer M5 ZLS does not encode an array stride.
            // The full data/metadata ranges were validated above.
            translated.parameters.depth_stride = 0;
            translated.parameters.depth_aux_stride = 0;
            translated.parameters.stencil_stride = 0;
            translated.parameters.stencil_aux_stride = 0;
            // Native 25G76 indexed draws use 15021=0x10c000. The reference
            // triangle's low bit causes fragment recovery with larger
            // indexed overdraw, even after the tiling stage completes.
            // Bit18 selects the narrow depth plane: the same controlled
            // render against a Depth16Unorm attachment publishes 0x14c001
            // where the 32-bit one publishes 0x10c001, and that bit is the
            // only difference in the whole register program beyond the ZLS
            // format field and the integer clear value.
            let z16 = payload.zls_control & (3 << 25) == (2 << 25);
            translated.parameters.aux_fb_flags =
                if z16 { 0x14c000 } else { 0x10c000 };
            // M5's second store-program pair refers to the same EOT program
            // in the admitted native profile, with caller-owned program VAs.
            controls.store_pipeline_2 = translated.parameters.store_pipeline;
            controls.store_pipeline_2_bind = translated.parameters.store_pipeline_bind;
            controls.partial_store_pipeline_2 = translated.parameters.partial_store_pipeline;
            controls.partial_store_pipeline_2_bind = translated.parameters.partial_store_pipeline_bind;
            let _busy = self.clpc.busy();
            self.runtime.lock().submit_application(&self.vm, translated.parameters,
                controls, 1, [0x7d4000, 0x934000])?;
            Ok(())
        })();
        // The runtime records and quarantines a failed published VM before
        // this output fence is installed/signalled by the publication guard.
        publication.result = result;
        result
    }
}

/// Admit the captured M5 single-layer render-state envelope. An occlusion
/// (visibility) buffer is admitted: translate range-checks it and it is
/// written to 0x15311 and the descriptor, and Apple's visibility renders move
/// only the ISP control and query-index words beside it
/// (gpu/g17g-witness/ppp/statedump.m occl=1/2). A sampler heap is admitted:
/// translate range-checks it and the descriptor carries it
/// (+0x87a/+0x882/+0x886 as on G17P). This is an experimental first-job ABI
/// path, not an assertion that arbitrary Vulkan programs are supported.
fn admit_reference_render(command: &command::UapiRenderCommand) -> Result {
    let sample_layout = matches!((command.utile_width, command.utile_height,
                                  command.samples),
                                 (32, 32, 1) | (32, 32, 2) | (32, 16, 4));
    let timestamps = [command.vertex_timestamps.start, command.vertex_timestamps.end,
        command.fragment_timestamps.start, command.fragment_timestamps.end];
    if !(1..=command::M5_MAX_RENDER_DIMENSION).contains(&u32::from(command.width))
        || !(1..=command::M5_MAX_RENDER_DIMENSION).contains(&u32::from(command.height))
        || command.layers != 1
        || !sample_layout
        || timestamps.iter().any(|timestamp| timestamp.handle != 0 || timestamp.offset != 0) {
        return Err(ENOTSUPP);
    }
    Ok(())
}
