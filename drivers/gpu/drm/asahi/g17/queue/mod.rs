// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Userspace submission queues: validate whole ioctls, retain mappings, and enqueue
//! independent engines behind user fences and explicit logical prefixes.

pub(crate) mod compute;
pub(crate) mod render;

use super::{
    command::{Parser, UscWindow, Validated},
    completion::Completion,
    context::Context,
    fence::{Outputs, Submission},
    job::{self, Backend, Engine, Fences, Order, Packet, Scheduler},
    validation::Proof,
};
use crate::{
    driver::{AsahiDevRef, AsahiDevice},
    file, mmu,
};
use core::ops::Range;
use kernel::{
    c_str,
    dma_fence::FenceContexts,
    prelude::*,
    sync::{Arc, LockClassKey},
    xarray,
};

const EARLY_DEPTH: usize = 2;
static FENCE_KEY: Pin<&LockClassKey> = kernel::static_lock_class!();

pub(crate) mod admission;

/// Host identities for retained physical channels, separate from firmware object generations.
/// The device mutex protects allocation; cancelled preparations never reuse IDs.
pub(crate) struct Identities(u64);

impl Identities {
    pub(crate) const fn new() -> Self {
        Self(3)
    }

    /// The first two identities remain reserved with QIDs 0 and 1.
    pub(crate) fn reserve(&mut self, count: u64) -> Result<Range<u64>> {
        let first = self.0;
        let end = first.checked_add(count).ok_or(EOVERFLOW)?;
        self.0 = end;
        Ok(first..end)
    }
}

/// Validated frontend state prepared before allocating a logical execution context.
pub(crate) struct Frontend {
    fences: FenceContexts,
    range: Range<u64>,
    window: UscWindow,
}

impl Frontend {
    pub(crate) fn new(priority: u32, usc_base: u64, range: Range<u64>) -> Result<Self> {
        if priority > 3 {
            return Err(EINVAL);
        }
        let window = UscWindow::new(usc_base, range.clone())?;
        let fences = FenceContexts::new(1, c_str!("asahi_g17_queue"), FENCE_KEY)?;
        Ok(Self {
            fences,
            range,
            window,
        })
    }
}

pub(crate) struct Queue<B: Backend> {
    dev: AsahiDevRef,
    context: Arc<Context>,
    backend: Arc<B>,
    render: Option<Scheduler<B>>,
    compute: Option<Scheduler<B>>,
    fences: FenceContexts,
    range: Range<u64>,
    window: UscWindow,
    proof: Proof,
    sequence: [u64; 2],
    frontiers: [KVec<(u64, Fences)>; 2],
    // Closing the userspace queue releases admission even if firmware still
    // retains one of its execution contexts or installed physical graphs.
    _slot: admission::QueueSlot,
}

impl<B: Backend> Queue<B> {
    pub(crate) fn new(
        dev: &AsahiDevice,
        context: Arc<Context>,
        backend: Arc<B>,
        frontend: Frontend,
        slot: admission::QueueSlot,
    ) -> Result<Self> {
        let Frontend {
            fences,
            range,
            window,
        } = frontend;
        let render = Some(Scheduler::new(dev, backend.clone(), Engine::Render)?);
        Ok(Self {
            dev: dev.into(),
            context,
            backend,
            render,
            compute: None,
            fences,
            range,
            window,
            proof: Proof::new(),
            sequence: [0; 2],
            frontiers: core::array::from_fn(|_| KVec::new()),
            _slot: slot,
        })
    }

    fn ensure_compute(&mut self) -> Result {
        // Admission is checked even when this queue already owns a scheduler.
        self.backend.ensure_compute()?;
        if self.compute.is_none() {
            self.compute = Some(Scheduler::new(
                &self.dev,
                self.backend.clone(),
                Engine::Compute,
            )?);
        }
        Ok(())
    }

    fn enqueue(
        &mut self,
        id: u64,
        command: Validated,
        order: Order,
        guard: mmu::VmJobGuard,
        aggregate: &Arc<Submission>,
        inputs: &[file::SyncItem],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        let engine = match &command {
            Validated::Render { .. } => Engine::Render,
            Validated::Compute { .. } => Engine::Compute,
        };
        let index = engine.index();
        self.frontiers[index].retain(|(_, fences)| job::fence_status(&fences.completed) == 0);
        self.frontiers[index].reserve(1, GFP_KERNEL)?;
        if engine == Engine::Compute {
            self.ensure_compute()?;
        }
        let completion = match &command {
            Validated::Render { timestamps, .. } => Completion::new(
                &self.dev,
                &self.fences,
                &self.context,
                aggregate.member(),
                guard,
                objects,
                timestamps,
                false,
                self.backend.feed(),
            )?,
            Validated::Compute { timestamps, .. } => Completion::new(
                &self.dev,
                &self.fences,
                &self.context,
                aggregate.member(),
                guard,
                objects,
                core::slice::from_ref(timestamps),
                true,
                None,
            )?,
        };
        if engine == Engine::Compute {
            self.frontiers[Engine::Render.index()]
                .retain(|(_, fences)| job::fence_status(&fences.completed) == 0);
        }
        let other = 1 - index;
        let prefix = order.wait_through[other];
        let frontier = if prefix.is_some() {
            let frontier = self.frontiers[other].as_slice();
            // Accepted sequences increase from one, and retain preserves their
            // order, so all members of a logical prefix form one slice.
            let end = frontier.partition_point(|(sequence, _)| Order::contains(prefix, *sequence));
            &frontier[..end]
        } else {
            &[]
        };
        let count = frontier.len();
        let mut dependencies = KVec::with_capacity(inputs.len() + count, GFP_KERNEL)?;
        let mut checked_inputs = KVec::with_capacity(inputs.len(), GFP_KERNEL)?;
        let mut firmware = KVec::with_capacity(count, GFP_KERNEL)?;
        for (_, fences) in frontier {
            dependencies.push(fences.ready.clone(), GFP_KERNEL)?;
            firmware.push(fences.completed.clone(), GFP_KERNEL)?;
        }
        // Each command owns every input fence; independent engines may publish in
        // either order and must each consume an input's error status before running.
        for sync in inputs {
            if let Some(fence) = &sync.fence {
                dependencies.push(fence.clone(), GFP_KERNEL)?;
                checked_inputs.push(fence.clone(), GFP_KERNEL)?;
            }
        }
        let early = engine == Engine::Render
            && dependencies.is_empty()
            && self.frontiers[index].len() < EARLY_DEPTH;
        let packet = Packet::new(
            id,
            self.context.clone(),
            command,
            order,
            completion,
            checked_inputs,
            firmware,
        )?;
        let scheduler = match engine {
            Engine::Render => &mut self.render,
            Engine::Compute => &mut self.compute,
        };
        let fences = scheduler
            .as_mut()
            .ok_or(ENODEV)?
            .enqueue(packet, dependencies, early)?;
        self.sequence[index] = order.sequence;
        self.frontiers[index]
            .push_within_capacity((order.sequence, fences))
            .map_err(|_| ENOMEM)?;
        Ok(())
    }
    /// Finish parsing, validation and VM pinning before entering publication.
    /// Keeping this frame separate bounds the scheduler/ioctl stack depth.
    #[inline(never)]
    fn prepare_submission(
        &mut self,
        bytes: &[u8],
    ) -> Result<(KVec<(Validated, Order)>, KVec<mmu::VmJobGuard>)> {
        let parse_guard = self.context.vm().retain_first_job()?;
        let mut parser = Parser::new(bytes);
        let mut preceding = [0; 2];
        let mut pending = KVec::with_capacity(1, GFP_KERNEL)?;
        while let Some(command) = parser.next()? {
            let payload = self.proof.validate(
                self.context.vm(),
                self.range.clone(),
                &self.window,
                &command.payload,
            )?;
            let engine = match &payload {
                Validated::Render { .. } => Engine::Render,
                Validated::Compute { .. } => Engine::Compute,
            };
            let order = Order::new(self.sequence, preceding, command.barriers, engine)?;
            preceding[engine.index()] += 1;
            pending.push((payload, order), GFP_KERNEL)?;
        }
        parser.finish()?;
        // Every accepted command pins the VM before any command is enqueued.
        let mut guards = KVec::with_capacity(pending.len(), GFP_KERNEL)?;
        guards
            .push_within_capacity(parse_guard)
            .map_err(|_| ENOMEM)?;
        while guards.len() < pending.len() {
            guards
                .push_within_capacity(self.context.vm().retain_job()?)
                .map_err(|_| ENOMEM)?;
        }
        Ok((pending, guards))
    }
}

impl<B: Backend> crate::queue::Queue for Queue<B> {
    fn submit(
        &mut self,
        id: u64,
        syncs: KVec<file::SyncItem>,
        input_count: usize,
        bytes: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        let mut outputs = Outputs::new(&self.fences, syncs, input_count)?;
        let result = (|| {
            if self.context.status().get() != 0 {
                return Err(EIO);
            }
            let (pending, guards) = self.prepare_submission(bytes)?;
            let mut guards = guards.into_iter();
            let aggregate = outputs.publish(self.context.clone())?;
            for (command, order) in pending {
                let guard = guards.next().ok_or(EIO)?;
                self.enqueue(
                    id,
                    command,
                    order,
                    guard,
                    &aggregate,
                    outputs.inputs(),
                    objects,
                )?;
            }
            Ok(())
        })();
        if let Err(error) = result {
            self.context.status().record_if_device_loss(error);
            if [EIO, ETIMEDOUT, ENODEV].contains(&error) || self.context.status().get() != 0 {
                self.backend.fail_vm(error);
            }
        }
        outputs.finish(result)
    }
}

impl<B: Backend> Drop for Queue<B> {
    fn drop(&mut self) {
        drop(self.render.take());
        drop(self.compute.take());
        self.backend.release_owner();
    }
}
