// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Userspace submission queues: validate whole ioctls, retain mappings, and enqueue
//! independent engines behind user fences and explicit logical prefixes.

pub(crate) mod compute;
pub(crate) mod render;

use super::{
    command::{Parser, UscWindow, Validated},
    completion::{Completion, Destinations, PreparedPublication},
    context::Context,
    dependency::FailureRecord,
    fence::Outputs,
    job::{self, Backend, Engine, Fences, Order, Packet, Scheduler},
    validation::Proof,
};
use crate::{
    driver::{AsahiDevRef, AsahiDrmDevice},
    file, mmu,
};
use core::ops::Range;
use kernel::{
    c_str,
    dma_fence::{Fence, FenceContexts},
    prelude::*,
    sync::{Arc, LockClassKey},
    xarray,
};

static FENCE_KEY: Pin<&LockClassKey> = kernel::static_lock_class!();

pub(crate) mod admission;
mod outstanding;

use outstanding::Outstanding;

impl outstanding::Status for Fences {
    fn status(&self) -> i32 {
        Fences::status(self)
    }
}

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
    allocation_failure: Fence,
    range: Range<u64>,
    window: UscWindow,
    variant: &'static crate::g17::fw::Variant,
}

impl Frontend {
    pub(crate) fn new(
        priority: u32,
        usc_base: u64,
        range: Range<u64>,
        variant: &'static crate::g17::fw::Variant,
    ) -> Result<Self> {
        if priority > 3 {
            return Err(EINVAL);
        }
        let window = UscWindow::new(usc_base, range.clone())?;
        let fences = FenceContexts::new(1, c_str!("aurora_g17_queue"), FENCE_KEY)?;
        let allocation_failure = super::fence::allocation_failure(&fences)?;
        Ok(Self {
            fences,
            allocation_failure,
            range,
            window,
            variant,
        })
    }
}

pub(crate) struct Queue<B: Backend> {
    dev: AsahiDevRef,
    context: Arc<Context>,
    /// This queue's engine time: the union of its overlapping stages, charged to its file.
    usage: Arc<crate::usage::Lane<Arc<crate::usage::Client>>>,
    backend: Arc<B>,
    render: Option<Scheduler<B>>,
    compute: Option<Scheduler<B>>,
    /// The watchdog allowance of each compute command, set before the first one
    /// ([`crate::queue::Queue::set_compute_timeout`]).
    compute_allowance_ms: u32,
    fences: FenceContexts,
    allocation_failure: Fence,
    range: Range<u64>,
    window: UscWindow,
    variant: &'static crate::g17::fw::Variant,
    proof: Proof,
    sequence: [u64; 2],
    /// Each engine's unfinished commands, as many as userspace keeps queued (thousands
    /// under vkd3d-proton).
    frontiers: [Outstanding<Fences>; 2],
    /// The frontiers' host-timestamp publishers, again: the only entries the host-publication
    /// links and the compute timestamp waits look for. Few commands publish, so those readers
    /// walk this instead of every unfinished command.
    publishers: [Outstanding<Fences>; 2],
    /// Finished frontier entries pruned during this ioctl. Their last references
    /// can tear a whole completion down; that waits until the ioctl has published.
    /// Like the frontiers it needs no contiguous pages, and its growth must not fail
    /// a submit on fragmentation.
    finished: KVVec<Fences>,
    failures: [FailureRecord<Fences>; 2],
    // Closing the userspace queue releases admission even if firmware still
    // retains one of its execution contexts or installed physical graphs.
    _slot: admission::QueueSlot,
}

type ValidatedBatch = (
    KVec<(Validated, Order, Destinations)>,
    KVec<mmu::VmJobGuard>,
);

impl<B: Backend> Queue<B> {
    pub(crate) fn new(
        dev: &AsahiDrmDevice,
        context: Arc<Context>,
        backend: Arc<B>,
        frontend: Frontend,
        slot: admission::QueueSlot,
        usage: Arc<crate::usage::Client>,
    ) -> Result<Self> {
        let Frontend {
            fences,
            allocation_failure,
            range,
            window,
            variant,
        } = frontend;
        let render = Some(Scheduler::new(
            dev,
            backend.clone(),
            Engine::Render,
            super::timeout::DEFAULT_ALLOWANCE_MS,
        )?);
        let failures = context.status().failures().finished;
        Ok(Self {
            dev: dev.into(),
            context,
            usage: Arc::new(crate::usage::Lane::new(usage), GFP_KERNEL)?,
            backend,
            render,
            compute: None,
            compute_allowance_ms: super::timeout::DEFAULT_ALLOWANCE_MS,
            fences,
            allocation_failure,
            range,
            window,
            variant,
            proof: Proof::new(),
            sequence: [0; 2],
            frontiers: [Outstanding::new(failures), Outstanding::new(failures)],
            publishers: [Outstanding::new(failures), Outstanding::new(failures)],
            finished: KVVec::new(),
            failures: [FailureRecord::new(), FailureRecord::new()],
            _slot: slot,
        })
    }

    fn ensure_compute(&mut self, command: &Validated) -> Result {
        // Validate every request, including an existing scheduler. Physical
        // admission happens after its DRM dependencies, in backend publication.
        self.backend.ensure_compute(command)?;
        if self.compute.is_none() {
            self.compute = Some(Scheduler::new(
                &self.dev,
                self.backend.clone(),
                Engine::Compute,
                self.compute_allowance_ms,
            )?);
        }
        Ok(())
    }

    fn prune_frontier(&mut self, index: usize) {
        outstanding::prune_engine(
            &mut self.frontiers[index],
            &mut self.publishers[index],
            self.context.status().failures(),
            &mut self.failures[index],
            &mut self.finished,
        );
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "Enqueue keeps validated command, ordering, and destination owners explicit."
    )]
    fn enqueue(
        &mut self,
        id: u64,
        command: Validated,
        order: Order,
        guard: mmu::VmJobGuard,
        aggregate: &super::fence::Aggregate,
        inputs: &[file::SyncItem],
        destinations: Destinations,
    ) -> Result {
        let engine = Engine::of(&command);
        let index = engine.index();
        if let Validated::Render { ray_query, .. } = &command {
            let blocks = self
                .backend
                .render_scs_blocks(ray_query.map(|stage| stage.scratch.pages()))?;
            if blocks > self.context.render_blocks() {
                return Err(EINVAL);
            }
        }
        self.prune_frontier(index);
        if engine == Engine::Compute {
            self.ensure_compute(&command)?;
        }
        // Only convert inputs when no independent host-prefix wait could keep
        // the retained source bookkeeping queued behind an external signal.
        if engine == Engine::Compute {
            super::trace::inputs(self.context.id(), order.sequence, 0, inputs.len(), None);
        }
        // Ring order completes a converted input only if a kick waits for its
        // ring predecessor; without that, keep the input's completion wait.
        let host_waits = self.backend.host_producer_waits();
        let sources = if !host_waits
            && engine == Engine::Compute
            && !inputs.is_empty()
            && !destinations.has_outputs()
            && self.frontiers[0]
                .prefix(order.wait_through[0])
                .all(|(_, fences)| fences.status() > 0)
            && self.publishers[1]
                .prefix(order.wait_through[1])
                .all(|(_, fences)| fences.status() > 0)
            && self.failures.iter_mut().enumerate().all(|(index, failure)| {
                failure.earliest().is_none_or(|(sequence, _)| {
                    !Order::contains(order.wait_through[index], sequence)
                })
            }) {
            self.backend.input_sources(inputs)
        } else {
            None
        };
        if engine == Engine::Compute {
            super::trace::inputs(
                self.context.id(),
                order.sequence,
                1,
                inputs.len(),
                sources.as_deref(),
            );
        }

        // Only earlier render ordinals covered by the explicit logical prefix
        // can delay host publication. These links never become GPU dependencies.
        // A finished one may still be held behind an unfinished one: it delays nothing.
        let host_prefix = if engine == Engine::Render { order.wait_through[index] } else { None };
        let host_predecessors = self.publishers[index]
            .prefix(host_prefix)
            .filter_map(|(_, fences)| fences.publication.as_ref().filter(|_| fences.status() <= 0));
        let completion = Completion::new(
            &self.dev,
            &self.fences,
            &self.context,
            aggregate.member(),
            guard,
            destinations,
            engine == Engine::Compute,
            host_predecessors.clone().next().is_some(),
            self.backend.feed(),
            sources.clone(),
            self.usage.clone(),
        )?;
        let publication = completion
            .publication()
            .map(|target| PreparedPublication::new(target, host_predecessors))
            .transpose()?;
        if engine == Engine::Compute {
            self.prune_frontier(Engine::Render.index());
        }
        let other = 1 - index;
        let prefix = order.wait_through[other];
        let frontier = self.frontiers[other].prefix(prefix);
        // Finished producers are skipped below. Those still held behind an unfinished one are
        // left out here too, so the room below is not sized by them.
        let count = frontier.clone().filter(|(_, fences)| fences.status() <= 0).count();
        // Only a command that copies or resets queries can observe timestamps the
        // host has not yet published for its predecessors.
        let query_access = match &command {
            Validated::Compute { query_access, .. } => *query_access,
            Validated::Render { .. } => true,
        };
        let timestamp_prefix = if engine == Engine::Compute && query_access {
            order.wait_through[index]
        } else {
            None
        };
        // A finished publisher may still be held behind an unfinished one: its timestamps
        // are written, so there is nothing to wait for.
        let pending_publisher = |fences: &Fences| fences.status() <= 0;
        let timestamps = self.publishers[index].prefix(timestamp_prefix);
        let timestamp_count = timestamps
            .clone()
            .filter(|(_, fences)| pending_publisher(fences))
            .count();
        let mut failures: [Option<Fence>; 2] = [None, None];
        for (index, failure) in self.failures.iter_mut().enumerate() {
            failures[index] = failure.earliest().and_then(|(sequence, fences)| {
                Order::contains(order.wait_through[index], sequence).then(|| fences.completed.clone())
            });
        }
        let failure_count = failures.iter().flatten().count();
        let mut dependencies = KVec::with_capacity(
            inputs.len() + count + timestamp_count + failure_count,
            GFP_KERNEL,
        )?;
        // Host-waited producers are checked on the host instead of in firmware.
        let (host_count, firmware_count) = if host_waits {
            (count, 0)
        } else {
            (0, count)
        };
        let mut checked_inputs = KVec::with_capacity(
            inputs.len() + host_count + timestamp_count + failure_count,
            GFP_KERNEL,
        )?;
        let mut firmware = KVec::with_capacity(firmware_count, GFP_KERNEL)?;
        for fence in failures.into_iter().flatten() {
            checked_inputs.push(fence.clone(), GFP_KERNEL)?;
            dependencies.push(fence.clone(), GFP_KERNEL)?;
        }
        // Same-engine parents order GPU execution, not the host's timestamp
        // writes. Only timestamp publishers need these completion waits and
        // status checks; ordinary compute keeps its existing queue ordering.
        for (_, fences) in timestamps {
            if pending_publisher(fences) {
                checked_inputs.push(fences.completed.clone(), GFP_KERNEL)?;
                dependencies.push(fences.ready.clone(), GFP_KERNEL)?;
            }
        }
        for (_, fences) in frontier {
            // Successful completion includes host timestamp publication. Its
            // ready fence adds no ordering and would prevent shallow inline
            // publication. Failed and pending producers still need checking.
            if fences.status() > 0 {
                continue;
            }
            if host_waits {
                // Keep other-engine producers on the host until completion, and
                // propagate their errors before publishing the consumer.
                dependencies.push(fences.completed.clone(), GFP_KERNEL)?;
                checked_inputs.push(fences.completed.clone(), GFP_KERNEL)?;
            } else {
                dependencies.push(fences.ready.clone(), GFP_KERNEL)?;
                firmware.push(fences.completed.clone(), GFP_KERNEL)?;
            }
        }
        // Each command owns every input fence; independent engines may publish in
        // either order and must each consume an input's error status before running.
        for sync in inputs {
            if let Some(fence) = &sync.fence {
                // An input that already signalled without error orders nothing and has no status
                // left to consume (like a completed frontier producer above). Keeping it would
                // send the render through the scheduler thread instead of inline publication:
                // most compositor and client renders carry such already-signalled buffer fences.
                if job::fence_status(fence) > 0 {
                    continue;
                }
                if sources
                    .as_ref()
                    .is_some_and(|sources| sources.contains_input(fence))
                {
                    continue;
                }
                dependencies.push(fence.clone(), GFP_KERNEL)?;
                checked_inputs.push(fence.clone(), GFP_KERNEL)?;
            }
        }
        if let Some(sources) = &sources {
            for ready in sources.publication_waits() {
                dependencies.push(ready.clone(), GFP_KERNEL)?;
                checked_inputs.push(ready.clone(), GFP_KERNEL)?;
            }
        }
        // Publish from the ioctl whenever nothing has to wait, at any depth.
        // The backend falls back to the scheduler behind any backlog or on any
        // capacity miss, so submission order is preserved.
        let early = dependencies.is_empty();
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
        let fences =
            scheduler
                .as_mut()
                .ok_or(ENODEV)?
                .enqueue(packet, dependencies, early, publication)?;
        self.sequence[index] = order.sequence;
        if fences.publishes_timestamps() {
            self.publishers[index].push_within_capacity(order.sequence, fences.clone())?;
        }
        self.frontiers[index].push_within_capacity(order.sequence, fences)?;
        Ok(())
    }
    /// Finish parsing, validation and VM pinning before entering publication.
    /// Keeping this frame separate bounds the scheduler/ioctl stack depth.
    #[inline(never)]
    fn prepare_submission(
        &mut self,
        bytes: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result<ValidatedBatch> {
        // A refused submission reaches userspace as EINVAL alone, so name the step that
        // refused it: the command's own requirements are only one of several.
        let refused = |step: &str, error: kernel::error::Error| {
            static REFUSED: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
            if REFUSED.fetch_add(1, core::sync::atomic::Ordering::Relaxed) < 8 {
                pr_warn!("G17: refused a submission at {}: {:?}\n", step, error);
            }
            error
        };
        let parse_guard = self
            .context
            .vm()
            .retain_first_job()
            .map_err(|error| refused("pinning the VM", error))?;
        let mut parser = Parser::new(bytes);
        // All historical ordinals use the same entry counters; publication
        // starts only after the entire ioctl has been validated and pinned.
        let base = self.sequence;
        let mut preceding = [0; 2];
        let mut pending = KVec::with_capacity(1, GFP_KERNEL)?;
        while let Some(command) = parser.next()? {
            let mut payload = self
                .proof
                .validate(
                    self.context.vm(),
                    self.range.clone(),
                    &self.window,
                    self.variant,
                    &command.payload,
                )
                .map_err(|error| refused("the command's requirements", error))?;
            let engine = Engine::of(&payload);
            if !self.backend.supports_engine(engine) { return Err(ENOTSUPP); }
            self.backend
                .prepare_command(&mut payload)
                .map_err(|error| refused("preparing the command", error))?;
            let mut order = Order::new(
                base,
                preceding,
                command.barriers,
                command.payload.prior(),
                engine,
            )
            .map_err(|error| refused("its barriers and priors", error))?;
            if let Some((barriers, prior)) = command.payload.tiling_prefixes() {
                let tiling = Order::new(base, preceding, barriers, prior, engine)
                    .map_err(|error| refused("its tiling prefixes", error))?;
                order
                    .narrow_tiling(&tiling)
                    .map_err(|error| refused("narrowing to its tiling prefixes", error))?;
            }
            preceding[engine.index()] += 1;
            let destinations = match &payload {
                Validated::Render { timestamps, .. } => Destinations::resolve(objects, timestamps),
                Validated::Compute { timestamps, .. } => {
                    Destinations::resolve(objects, core::slice::from_ref(timestamps))
                }
            }
            .map_err(|error| refused("its timestamp destinations", error))?;
            pending.push((payload, order, destinations), GFP_KERNEL)?;
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
    fn set_compute_timeout(&mut self, requested_ms: u32) -> Result<u32> {
        // The compute scheduler takes the allowance when the first compute command creates it.
        if self.compute.is_some() {
            return Err(EBUSY);
        }
        let maximum = crate::module_parameters::compute_timeout_max_ms.value();
        self.compute_allowance_ms = super::timeout::allowance_ms(requested_ms, maximum);
        Ok(self.compute_allowance_ms)
    }

    fn reserve_render_scs(&mut self, layouts: [u64; 2]) -> Result {
        let pages = [
            super::fw::compute::ScratchRequest::new(layouts[0])?.pages(),
            super::fw::compute::ScratchRequest::new(layouts[1])?.pages(),
        ];
        let blocks = self.backend.render_scs_blocks(pages)?;
        self.context.reserve_render_blocks(blocks)
    }

    fn submit(
        &mut self,
        _reg_data: &crate::driver::AsahiRegData<'_>,
        id: u64,
        syncs: KVec<file::SyncItem>,
        input_count: usize,
        bytes: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        let output_count = syncs.len().saturating_sub(input_count);
        super::trace::submit(0, id, self.context.id(), input_count, output_count, 0, None);
        let mut outputs = match Outputs::new(
            &self.fences,
            syncs,
            input_count,
            &self.allocation_failure,
        ) {
            Ok(outputs) => outputs,
            Err(error) => {
                // Allocation failure installs the queue's immutable error fence. A malformed
                // input/output split installs nothing because no output set is well-defined.
                let fence = (error != EINVAL).then_some(&self.allocation_failure);
                super::trace::submit(
                    2,
                    id,
                    self.context.id(),
                    input_count,
                    output_count,
                    error.to_errno(),
                    fence,
                );
                return Err(error);
            }
        };
        let result = (|| {
            if !self.backend.supports_submission() {
                return Err(ENOTSUPP);
            }
            if self.context.status().get() != 0 {
                return Err(EIO);
            }
            let (mut pending, guards) = self.prepare_submission(bytes, objects)?;
            if pending
                .iter()
                .any(|(command, _, _)| matches!(command, Validated::Render { .. }))
            {
                self.context.note_render();
            }
            if super::timeout::select_submission(pending.iter().map(|(cmd, _, _)| cmd.long_job())) {
                for (command, _, _) in pending.iter_mut() {
                    command.allow_long_job();
                }
            }
            // Room for every command's frontier entry, so that running out of memory fails
            // the submission before any of it is queued. Userspace sets the frontier's
            // length, so its memory is charged to the caller.
            let mut room = [0; 2];
            for (command, _, _) in pending.iter() {
                room[Engine::of(command).index()] += 1;
            }
            for (frontier, room) in self.frontiers.iter_mut().zip(room) {
                frontier.reserve(room)?;
            }
            for (publishers, room) in self.publishers.iter_mut().zip(room) {
                publishers.reserve(room)?;
            }
            let mut guards = guards.into_iter();
            // Where a firmware wait reports completion before a weak stream tail's writes are
            // visible, only a command that declares a visible completion is waited for there.
            let visible_required = self.backend.firmware_sources_need_visible_completion();
            let firmware_source = pending.len() == 1
                && matches!(&pending[0].0, Validated::Compute { visible_completion, .. }
                    if *visible_completion || !visible_required)
                && !pending[0].2.has_outputs();
            let aggregate = outputs.publish(self.context.clone(), firmware_source)?;
            let output_fence = aggregate.fence();
            super::trace::submit(
                1,
                id,
                self.context.id(),
                input_count,
                output_count,
                0,
                Some(&output_fence),
            );
            for (command, order, destinations) in pending {
                let guard = guards.next().ok_or(EIO)?;
                self.enqueue(
                    id,
                    command,
                    order,
                    guard,
                    &aggregate,
                    outputs.inputs(),
                    destinations,
                )?;
            }
            Ok(())
        })();
        // Everything this ioctl publishes is visible; release what it pruned.
        self.finished.clear();
        if let Err(error) = result {
            self.context.status().record_if_device_loss(error);
            if [EIO, ETIMEDOUT, ENODEV].contains(&error) || self.context.status().get() != 0 {
                self.backend.fail_vm(error);
            }
        }
        let output_fence = outputs.output_fence();
        super::trace::submit(
            2,
            id,
            self.context.id(),
            input_count,
            output_count,
            result.as_ref().err().map_or(0, |error| error.to_errno()),
            output_fence.as_ref(),
        );
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
