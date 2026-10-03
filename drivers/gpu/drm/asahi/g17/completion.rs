// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-command fences, timestamp destinations and VM pins retained through retirement.
//!
//! Runtime queues own completions before making work visible. They defer fence signalling
//! until after the device mutex is released, and retain failed visible work until a retirement
//! witness or processor stop. An error fence never substitutes for that ownership boundary.

use super::{
    context::{Context, WorkStateLease},
    fence::{self, CompletionFence, Member},
    object::{CpuMap, KernelObject},
    recovery::Class,
    status::VmStatus,
};
use crate::{driver::AsahiDevice, file, hw::t8140::resources as cfg, mmu};
use core::sync::atomic::{fence as barrier, AtomicBool, AtomicI32, AtomicU8, Ordering};
use kernel::{
    dma_fence::{Fence, FenceContexts, RawDmaFence, UserFence},
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
    uapi, xarray,
};

struct Destination {
    mapping: Arc<mmu::KernelMapping>,
    offset: usize,
}

impl Destination {
    fn resolve(
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: uapi::drm_asahi_timestamp,
    ) -> Result<Option<Self>> {
        if timestamp.handle == 0 {
            return Ok(None);
        }
        let object = objects
            .lock()
            .get(timestamp.handle.try_into()?)
            .ok_or(ENOENT)?
            .clone();
        let file::Object::TimestampBuffer(mapping) = object;
        let offset: usize = timestamp.offset.try_into()?;
        if offset.checked_add(size_of::<u64>()).ok_or(EINVAL)? > mapping.size() {
            return Err(ERANGE);
        }
        Ok(Some(Self { mapping, offset }))
    }

    fn write(&self, ticks: u64) -> Result {
        const NSEC_PER_SEC: u64 = 1_000_000_000;
        let ns = (ticks / cfg::COMMAND_TIMESTAMP_HZ)
            .wrapping_mul(NSEC_PER_SEC)
            .wrapping_add(
                ticks % cfg::COMMAND_TIMESTAMP_HZ * NSEC_PER_SEC / cfg::COMMAND_TIMESTAMP_HZ,
            );
        self.mapping.write_timestamp(self.offset, ns)?;
        barrier(Ordering::Release);
        Ok(())
    }
}

/// Firmware writes two aligned words in a fresh write-combined page per compute command.
struct ComputeTimestamps {
    // Remove the GPU alias before releasing its backing.
    mapping: mmu::KernelMapping,
    object: KernelObject,
}

impl ComputeTimestamps {
    fn new(dev: &AsahiDevice, vm: &mmu::Vm) -> Result<Self> {
        let object = KernelObject::backing(dev, mmu::UAT_PGSZ, CpuMap::WriteCombined)?;
        let mapping = object.alias_in(
            vm,
            cfg::TIMESTAMPS,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
        )?;
        Ok(Self { mapping, object })
    }
}

#[pin_data]
pub(crate) struct Completion {
    status: Arc<VmStatus>,
    fence: UserFence<CompletionFence>,
    member: Member,
    destinations: [Option<Destination>; 4],
    compute: Option<ComputeTimestamps>,
    feed: Option<Arc<super::feed::Feed>>,
    #[pin]
    vm_job: Mutex<Option<mmu::VmJobGuard>>,
    #[pin]
    render: Mutex<Option<super::queue::admission::RenderLease>>,
    signalled: AtomicBool,
    timeout_error: AtomicI32,
    replays: AtomicU8,
    replay_timeout: AtomicBool,
    firmware_consumer: AtomicBool,
    class: AtomicU8,
}

impl Completion {
    pub(crate) fn new(
        dev: &AsahiDevice,
        contexts: &FenceContexts,
        context: &Arc<Context>,
        member: Member,
        vm_job: mmu::VmJobGuard,
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamps: &[uapi::drm_asahi_timestamps],
        compute: bool,
        feed: Option<Arc<super::feed::Feed>>,
    ) -> Result<Arc<Self>> {
        if timestamps.len() != if compute { 1 } else { 2 } {
            return Err(EINVAL);
        }
        let mut destinations = core::array::from_fn(|_| None);
        for (index, timestamp) in timestamps.iter().enumerate() {
            destinations[2 * index] = Destination::resolve(objects, timestamp.start)?;
            destinations[2 * index + 1] = Destination::resolve(objects, timestamp.end)?;
        }
        let fence = fence::independent(contexts)?;
        let compute = if compute {
            Some(ComputeTimestamps::new(dev, context.vm())?)
        } else {
            None
        };
        Arc::pin_init(
            pin_init!(Self {
                status: context.status().clone(),
                fence,
                member,
                destinations,
                compute,
                feed,
                vm_job <- new_mutex!(Some(vm_job), "G17 command VM pin"),
                render <- new_mutex!(None, "G17 command render lease"),
                signalled: AtomicBool::new(false),
                timeout_error: AtomicI32::new(0),
                replays: AtomicU8::new(0),
                replay_timeout: AtomicBool::new(false),
                firmware_consumer: AtomicBool::new(false),
                class: AtomicU8::new(0),
            }),
            GFP_KERNEL,
        )
    }

    pub(crate) fn fence(&self) -> Fence {
        Fence::from_fence(&self.fence)
    }
    pub(crate) fn status(&self) -> &Arc<VmStatus> {
        &self.status
    }
    pub(crate) fn work_state(&self) -> Arc<WorkStateLease> {
        self.member.work_state()
    }
    pub(crate) fn has_timestamps(&self) -> bool {
        self.destinations.iter().any(Option::is_some)
    }

    pub(crate) fn compute_timestamp_va(&self) -> Result<u64> {
        Ok(self.compute.as_ref().ok_or(EINVAL)?.mapping.iova())
    }

    /// The caller has proved that firmware retired this command, or has
    /// received its scheduler kill acknowledgement with recovery closed.
    /// These are the command's public timestamp words, not queue-private state.
    pub(crate) fn compute_words(&self) -> Result<[u64; 2]> {
        let object = &self.compute.as_ref().ok_or(EINVAL)?.object;
        let words = [
            object.dword(0)?.load(Ordering::Relaxed),
            object.dword(8)?.load(Ordering::Relaxed),
        ];
        barrier(Ordering::Acquire);
        Ok(words)
    }

    pub(crate) fn replay_witness_words(&self) -> Option<[u64; 2]> {
        if self.signalled.load(Ordering::Acquire) {
            return None;
        }
        self.compute_words().ok()
    }

    pub(crate) fn begin_replay(&self) -> bool {
        let accepted = self
            .replays
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {
                (count < 3).then_some(count + 1)
            })
            .is_ok();
        if accepted {
            self.replay_timeout.store(true, Ordering::Release);
        }
        accepted
    }

    pub(crate) fn replay_count(&self) -> u8 {
        self.replays.load(Ordering::Acquire)
    }

    pub(crate) fn note_firmware_consumer(&self) {
        self.firmware_consumer.store(true, Ordering::Release);
    }

    pub(crate) fn has_firmware_consumer(&self) -> bool {
        self.firmware_consumer.load(Ordering::Acquire)
    }

    pub(crate) fn take_replay_timeout(&self) -> bool {
        !self.signalled.load(Ordering::Acquire) && self.replay_timeout.swap(false, Ordering::AcqRel)
    }

    /// Only a witnessed, retained command can be reset before republishing it.
    pub(crate) fn reset_for_replay(&self) -> Result {
        if self.signalled.load(Ordering::Acquire) {
            return Err(ECANCELED);
        }
        let object = &self.compute.as_ref().ok_or(EINVAL)?.object;
        let start = object.dword(0)?;
        let end = object.dword(8)?;
        start.store(0, Ordering::Relaxed);
        end.store(0, Ordering::Relaxed);
        barrier(Ordering::SeqCst);
        Ok(())
    }

    /// The first timeout request retains its errno while the physical graph is
    /// borrowed. Duplicate watchdog or cancellation callbacks cannot replace it.
    pub(crate) fn defer_timeout(&self, error: Error) {
        let _ = self.timeout_error.compare_exchange(
            0,
            error.to_errno(),
            Ordering::AcqRel,
            Ordering::Acquire,
        );
    }

    pub(crate) fn take_deferred_timeout(&self) -> Option<Error> {
        let error = self.timeout_error.swap(0, Ordering::AcqRel);
        (error != 0).then(|| Error::from_errno(error))
    }

    pub(crate) fn classify(&self, class: Class) {
        self.class.store(
            match class {
                Class::Spared => 1,
                Class::Guilty => 2,
            },
            Ordering::Release,
        );
    }

    pub(crate) fn spared(&self) -> bool {
        self.class.load(Ordering::Acquire) == 1
    }

    /// Installed before the first firmware-visible prefix. Cancellation takes
    /// the same lock after claiming the signal, so it cannot miss a late lease.
    pub(crate) fn attach_render_lease(
        &self,
        lease: super::queue::admission::RenderLease,
    ) -> Result {
        let mut render = self.render.lock();
        if self.signalled.load(Ordering::Acquire) {
            return Err(ECANCELED);
        }
        if render.is_some() {
            return Err(EIO);
        }
        *render = Some(lease);
        Ok(())
    }

    pub(crate) fn render_slot(&self) -> Option<u8> {
        self.render
            .lock()
            .as_ref()
            .map(super::queue::admission::RenderLease::slot)
    }

    /// Called under the device mutex before deferring the error signal.
    pub(crate) fn quarantine_render(&self) {
        if let Some(lease) = self.render.lock().as_ref() {
            lease.quarantine();
        }
    }

    /// An exact whole-pair drain or processor stop allows rehabilitation. The
    /// VM pin goes first; the still-held lease excludes reuse until both settle.
    pub(crate) fn release_after_retirement(&self) {
        drop(self.vm_job.lock().take());
        let lease = self.render.lock().take();
        if let Some(lease) = lease.as_ref() {
            lease.rehabilitate_after_firmware_drain();
        }
        drop(lease);
    }

    fn release_healthy(&self) {
        drop(self.render.lock().take());
        drop(self.vm_job.lock().take());
    }

    /// Exact retirement permits the physical lease and VM pin to be released
    /// before waking userspace. A failure fence alone never establishes this.
    pub(crate) fn complete(&self, result: Result<[u64; 4]>) {
        if self.signalled.swap(true, Ordering::AcqRel) {
            return;
        }
        if let (Some(feed), Ok(ticks)) = (&self.feed, &result) {
            feed.note_render_pass(ticks[2], ticks[3]);
        }
        let result = result.and_then(|ticks| {
            for (destination, ticks) in self.destinations.iter().zip(ticks) {
                if let Some(destination) = destination {
                    destination.write(ticks)?;
                }
            }
            Ok(())
        });
        if let Err(error) = result {
            if error != ECANCELED && !self.spared() {
                self.status.record(error);
            }
            self.fence.set_error(error);
        }
        self.release_healthy();
        self.fence.signal();
        self.member.complete(result);
    }

    /// A rejected, never-published command cannot poison the VM for contention.
    pub(crate) fn fail_unpublished(&self, error: Error) {
        if self.signalled.swap(true, Ordering::AcqRel) {
            return;
        }
        self.status.record_if_device_loss(error);
        self.fence.set_error(error);
        self.release_healthy();
        self.fence.signal();
        self.member.complete(Err(error));
    }

    /// The physical queue must already be quarantined. Keep mappings pinned while
    /// waking waiters, even if queue destruction requested cancellation.
    pub(crate) fn fail_while_owned(&self, error: Error) {
        if self.signalled.swap(true, Ordering::AcqRel) {
            return;
        }
        if let Some(guard) = self.vm_job.lock().as_ref() {
            guard.quarantine();
        }
        if error != ECANCELED && !self.spared() {
            self.status.record(error);
        }
        self.fence.set_error(error);
        self.fence.signal();
        self.member.complete(Err(error));
    }
}

/// Collected under the device mutex and executed only after releasing it.
/// Runtime queues establish the ownership witness before constructing `Retired`
/// or `Release`; `FailedOwned` leaves their physical and VM ownership intact.
pub(crate) enum Deferred {
    Retired(Arc<Completion>, Result<[u64; 4]>),
    Drained(Arc<Completion>, Result<[u64; 4]>),
    FailedOwned(Arc<Completion>, Error),
    Unpublished(Arc<Completion>, Error),
    Release(Arc<Completion>),
}

impl Deferred {
    pub(crate) fn finish(self) {
        match self {
            Self::Retired(completion, result) => {
                completion.complete(result);
                // An earlier failure may already have signalled this fence while
                // retaining the pin. Retirement is a separate release witness.
                completion.release_after_retirement();
            }
            Self::Drained(completion, result) => {
                completion.release_after_retirement();
                completion.complete(result);
            }
            Self::FailedOwned(completion, error) => completion.fail_while_owned(error),
            Self::Unpublished(completion, error) => completion.fail_unpublished(error),
            Self::Release(completion) => completion.release_after_retirement(),
        }
    }
}
