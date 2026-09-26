// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned command packets and DRM scheduler boundary for independent G17P queues.
//!
//! This component is deliberately separate from the qualified synchronous
//! frontend. Enabling it requires a backend with one retained hardware queue
//! owner per DRM queue and completion-driven retirement; the current shared
//! QID0/1/4 runtime does not satisfy that contract.

use core::sync::atomic::{fence, Ordering};
use kernel::{c_str, dma_fence::{Fence, UserFence, RawDmaFence}, drm::sched, prelude::*, sync::Arc};
use crate::{driver::AsahiDevice, g17_uapi, mmu};

/// Host-sealed bytes in a job-private command allocation. The builder consumes
/// its owning mapping, copies the encoded command, and exposes only its GPU
/// address afterward. Another job cannot restage this wrapper's allocation.
/// Additional address-space aliases of the same job-private GEM belong in the
/// packet's retained mappings, never in another queue's mutable command state.
pub(crate) struct G17PSealedCommand {
    mapping: mmu::KernelMapping,
    encoded_size: usize,
}

impl G17PSealedCommand {
    pub(crate) fn new(mapping: mmu::KernelMapping, encoded: &[u8]) -> Result<Self> {
        if encoded.is_empty() || encoded.len() > mapping.size() { return Err(EINVAL); }
        mapping.with_cpu_bytes(|bytes| {
            bytes.fill(0);
            bytes[..encoded.len()].copy_from_slice(encoded);
            Ok(())
        })?;
        fence(Ordering::Release);
        Ok(Self { mapping, encoded_size: encoded.len() })
    }

    pub(crate) fn gpu_address(&self) -> u64 { self.mapping.iova() }
    pub(crate) fn encoded_size(&self) -> usize { self.encoded_size }
}

pub(crate) struct TimestampDestination {
    pub(crate) mapping: Arc<mmu::KernelMapping>,
    pub(crate) offset: usize,
}

pub(crate) struct G17PJobCompletion {
    pub(crate) status: Arc<crate::g17_status::VmStatus>,
    pub(crate) fence: UserFence<crate::g17_drm::G17PSubmissionFence>,
    pub(crate) timestamps: mmu::KernelMapping,
    pub(crate) destinations: [Option<TimestampDestination>; 2],
}

impl G17PJobCompletion {
    pub(crate) fn gpu_address(&self) -> u64 { self.timestamps.iova() }
    pub(crate) fn fence(&self) -> Fence { Fence::from_fence(&self.fence) }
    pub(crate) fn complete(&self, result: Result<[u64; 2]>) {
        let result = result.and_then(|timestamps| {
            for (destination, timestamp) in self.destinations.iter().zip(timestamps) {
                if let Some(destination) = destination {
                    destination.mapping.with_cpu_bytes(|bytes| {
                        let end = destination.offset.checked_add(8).ok_or(EINVAL)?;
                        bytes.get_mut(destination.offset..end).ok_or(EINVAL)?
                            .copy_from_slice(&timestamp.to_le_bytes());
                        Ok(())
                    })?;
                }
            }
            Ok(())
        });
        if let Err(error) = result {
            // Publish sticky failure BEFORE the fence wakes userspace. A DRM
            // syncobj considers an error fence signaled, so waits query this.
            self.status.record(error.to_errno());
            self.fence.set_error(error);
        }
        self.fence.signal();
    }
}

/// Everything a prepared compute submission needs remains owned until its
/// hardware fence retires, including when its userspace queue is destroyed.
/// The context can be shared by jobs of one VM; different VMs own different
/// independently rooted leases. Command images are immutable after sealing.
pub(crate) struct G17PComputeJobPacket {
    submission_id: u64,
    hardware_queue_id: u8,
    context: Arc<mmu::T8140ComputeExecutionContext>,
    command: g17_uapi::TranslatedComputeCommand,
    command_images: KVec<G17PSealedCommand>,
    _retained_mappings: KVec<mmu::KernelMapping>,
    completion: Option<G17PJobCompletion>,
    _vm_job: Option<mmu::T8140VmJobGuard>,
}

impl G17PComputeJobPacket {
    pub(crate) fn new(
        submission_id: u64,
        hardware_queue_id: u8,
        context: Arc<mmu::T8140ComputeExecutionContext>,
        command: g17_uapi::TranslatedComputeCommand,
        command_images: KVec<G17PSealedCommand>,
        retained_mappings: KVec<mmu::KernelMapping>,
        completion: Option<G17PJobCompletion>,
        vm_job: Option<mmu::T8140VmJobGuard>,
    ) -> Result<Arc<Self>> {
        if submission_id == 0 || !(4..128).contains(&hardware_queue_id)
        { return Err(EINVAL); }
        Ok(Arc::new(Self {
            submission_id, hardware_queue_id, context, command, command_images,
            _retained_mappings: retained_mappings, completion, _vm_job: vm_job,
        }, GFP_KERNEL)?)
    }

    pub(crate) fn submission_id(&self) -> u64 { self.submission_id }
    pub(crate) fn hardware_queue_id(&self) -> u8 { self.hardware_queue_id }
    pub(crate) fn context(&self) -> &mmu::T8140ComputeExecutionContext { &self.context }
    pub(crate) fn command(&self) -> &g17_uapi::TranslatedComputeCommand { &self.command }
    pub(crate) fn command_images(&self) -> &[G17PSealedCommand] { &self.command_images }
    pub(crate) fn completion(&self) -> Result<&G17PJobCompletion> { self.completion.as_ref().ok_or(EINVAL) }
}

pub(crate) trait G17PQueueBackend: Send + Sync {
    fn prepare(&self, packet: &G17PComputeJobPacket) -> Option<Fence>;
    fn publish(&self, packet: Arc<G17PComputeJobPacket>) -> Result<Fence>;
    fn timed_out(&self, packet: Arc<G17PComputeJobPacket>) -> sched::Status;
    fn cancel(&self, packet: Arc<G17PComputeJobPacket>);
}

pub(crate) struct G17PScheduledJob<B: G17PQueueBackend> {
    backend: Arc<B>,
    packet: Arc<G17PComputeJobPacket>,
}

impl<B: G17PQueueBackend> sched::JobImpl for G17PScheduledJob<B> {
    fn prepare(job: &mut sched::Job<Self>) -> Option<Fence> {
        job.backend.prepare(&job.packet)
    }

    fn run(job: &mut sched::Job<Self>) -> Result<Option<Fence>> {
        // Return the unsignalled hardware fence immediately. DRM retains the
        // job's immutable packet while other independent queues can publish.
        job.backend.publish(job.packet.clone()).inspect_err(|error| {
            // DRM also signals its finished fence when run_job fails before
            // publication; that failure must be visible through the same API.
            if let Ok(completion) = job.packet.completion() {
                completion.status.record(error.to_errno());
            }
        }).map(Some)
    }

    fn timed_out(job: &mut sched::Job<Self>) -> sched::Status {
        job.backend.timed_out(job.packet.clone())
    }

    fn cancel(job: &mut sched::Job<Self>) {
        job.backend.cancel(job.packet.clone());
    }
}

/// One scheduler/entity per DRM queue, as on M1/M2. Dependencies are handed to
/// DRM instead of synchronously waited under a hardware lease. Queue hardware
/// capacity is a credit limit plus backend prepare fences, not a global lock.
pub(crate) struct G17PJobScheduler<B: G17PQueueBackend> {
    entity: sched::Entity<G17PScheduledJob<B>>,
    _scheduler: sched::Scheduler<G17PScheduledJob<B>>,
    backend: Arc<B>,
}

impl<B: G17PQueueBackend> G17PJobScheduler<B> {
    pub(crate) fn new(dev: &AsahiDevice, backend: Arc<B>, credits: u32) -> Result<Self> {
        if credits == 0 { return Err(EINVAL); }
        let scheduler = sched::Scheduler::new(
            dev.as_ref(), 1, credits, 0, 2000, c_str!("asahi_g17p_sched"),
        )?;
        let entity = sched::Entity::new(&scheduler, sched::Priority::Kernel)?;
        Ok(Self { entity, _scheduler: scheduler, backend })
    }

    pub(crate) fn enqueue(
        &mut self, packet: Arc<G17PComputeJobPacket>, dependencies: KVec<Fence>,
    ) -> Result<Fence> {
        let mut job = self.entity.new_job(1, G17PScheduledJob {
            backend: self.backend.clone(), packet,
        })?;
        for dependency in dependencies { job.add_dependency(dependency)?; }
        let mut job = job.arm();
        let finished = job.fences().finished();
        job.push();
        Ok(finished)
    }
}
