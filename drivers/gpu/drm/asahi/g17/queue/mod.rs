// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Userspace submission queues: validate whole ioctls, retain mappings, and enqueue
//! independent engines behind user fences and explicit logical prefixes.

pub(crate) mod compute;
pub(crate) mod render;

use super::{
    command::{Parser, UscWindow, Validated},
    completion::{Completion, Destinations, PreparedPublication, Publication},
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
    dma_fence::{Fence, FenceContexts, RawDmaFence},
    prelude::*,
    sync::{Arc, LockClassKey},
    xarray,
};

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
    frontiers: [KVec<(u64, Fences)>; 2],
    /// Finished frontier entries pruned during this ioctl. Their last references
    /// can tear a whole completion down; that waits until the ioctl has published.
    finished: KVec<Fences>,
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
            frontiers: core::array::from_fn(|_| KVec::new()),
            finished: KVec::new(),
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
        let failure = &mut self.failures[index];
        // Without room the entries are released here, as they always could be.
        let mut finished = if self
            .finished
            .reserve(self.frontiers[index].len(), GFP_KERNEL)
            .is_ok()
        {
            Some(&mut self.finished)
        } else {
            None
        };
        self.frontiers[index].retain(|(sequence, fences)| {
            let status = fences.status();
            if status == 0 {
                return true;
            }
            if let Some(finished) = finished.as_mut() {
                let _ = finished.push_within_capacity(fences.clone());
            }
            failure.note(*sequence, status, fences);
            false
        });
    }

    fn has_pending_publication(&self, order: &Order) -> bool {
        self.frontiers.iter().enumerate().any(|(index, frontier)| {
            frontier.iter().any(|(sequence, fences)| {
                Order::contains(order.wait_through[index], *sequence)
                    && fences.publication.is_some()
                    && fences.status() <= 0
            })
        })
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
        let engine = match &command {
            Validated::Render { .. } => Engine::Render,
            Validated::Compute { .. } => Engine::Compute,
        };
        let index = engine.index();
        let other = 1 - index;
        if let Validated::Render { ray_query, .. } = &command {
            let blocks = self
                .backend
                .render_scs_blocks(ray_query.map(|stage| stage.scratch.pages()))?;
            if blocks > self.context.render_blocks() {
                return Err(EINVAL);
            }
        }
        self.prune_frontier(index);
        self.prune_frontier(other);
        self.frontiers[index].reserve(1, GFP_KERNEL)?;
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
        let has_outputs = destinations.has_outputs();
        let query_access = match &command {
            Validated::Compute { query_access, .. } => *query_access,
            Validated::Render { .. } => true,
        };
        let sources = if !host_waits
            && engine == Engine::Compute
            && !inputs.is_empty()
            && !has_outputs
            && self.frontiers[0].iter().all(|(sequence, fences)| {
                !Order::contains(order.wait_through[0], *sequence) || fences.status() > 0
            })
            && self.frontiers[1].iter().all(|(sequence, fences)| {
                !Order::contains(order.wait_through[1], *sequence)
                    || (fences.ready.raw() != fences.completed.raw() && fences.publication.is_none())
                    || fences.status() > 0
            })
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

        let (completion, publication, ordering_host_waits, ordering_firmware_waits) = {
            let ordering = EnqueueOrdering::build(
                &self.frontiers[index],
                &self.frontiers[other],
                order,
                engine,
                host_waits,
                has_outputs,
                query_access,
            )?;
            let completion = Completion::new(
                &self.dev,
                &self.fences,
                &self.context,
                aggregate.member(),
                guard,
                destinations,
                engine == Engine::Compute,
                !ordering.predecessors.is_empty(),
                self.backend.feed(),
                sources.clone(),
                self.usage.clone(),
            )?;
            let publication = completion
                .publication()
                .map(|target| PreparedPublication::new(target, ordering.predecessors.iter().copied()))
                .transpose()?;
            (
                completion,
                publication,
                ordering.host_waits,
                ordering.firmware_waits,
            )
        };
        let mut failures: [Option<Fence>; 2] = [None, None];
        for (index, failure) in self.failures.iter_mut().enumerate() {
            failures[index] = failure.earliest().and_then(|(sequence, fences)| {
                Order::contains(order.wait_through[index], sequence).then(|| fences.completed.clone())
            });
        }
        let failure_count = failures.iter().flatten().count();
        let mut dependencies = KVec::with_capacity(
            inputs.len()
                + ordering_host_waits.len()
                + ordering_firmware_waits.len()
                + failure_count,
            GFP_KERNEL,
        )?;
        // Host-waited producers are checked on the host instead of in firmware.
        let mut checked_inputs = KVec::with_capacity(
            inputs.len() + ordering_host_waits.len() + failure_count,
            GFP_KERNEL,
        )?;
        let mut firmware = KVec::with_capacity(ordering_firmware_waits.len(), GFP_KERNEL)?;
        for fence in failures.into_iter().flatten() {
            checked_inputs.push(fence.clone(), GFP_KERNEL)?;
            dependencies.push(fence.clone(), GFP_KERNEL)?;
        }
        for fence in ordering_host_waits {
            dependencies.push(fence.clone(), GFP_KERNEL)?;
            checked_inputs.push(fence, GFP_KERNEL)?;
        }
        for (scheduled, completed) in ordering_firmware_waits {
            dependencies.push(scheduled, GFP_KERNEL)?;
            firmware.push(completed, GFP_KERNEL)?;
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
            let engine = match &payload {
                Validated::Render { .. } => Engine::Render,
                Validated::Compute { .. } => Engine::Compute,
            };
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
            let mut guards = guards.into_iter();
            // Where a firmware wait reports completion before a weak stream tail's writes are
            // visible, only a command that declares a visible completion is waited for there.
            let visible_required = self.backend.firmware_sources_need_visible_completion();
            let firmware_source = pending.len() == 1
                && matches!(&pending[0].0, Validated::Compute { visible_completion, .. }
                    if *visible_completion || !visible_required)
                && !pending[0].2.has_outputs()
                && !self.has_pending_publication(&pending[0].1);
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

/// Whether a cross-engine consumer must wait on the host for a covered
/// producer's completion fence before scheduling.
const fn cross_engine_wait_completed(
    host_waits: bool,
    has_outputs: bool,
    engine: Engine,
    query_access: bool,
    has_publication: bool,
) -> bool {
    if host_waits {
        return true;
    }
    if !has_publication {
        return false;
    }
    has_outputs || (matches!(engine, Engine::Compute) && query_access)
}

/// Whether a cross-engine consumer overlaps execution with a producer that has
/// ordered host publication and therefore chains its completion behind it.
const fn cross_engine_chain_publication(
    host_waits: bool,
    has_outputs: bool,
    engine: Engine,
    query_access: bool,
) -> bool {
    !host_waits && !has_outputs && !(matches!(engine, Engine::Compute) && query_access)
}

/// Whether a same-engine consumer must wait on the host for covered same-engine
/// predecessors that have ordered host publication: query-accessing compute may
/// read or write, on the GPU, query words whose host publication is still
/// pending, and timestamp-writing renders can write their fragment-end
/// timestamp window on the GPU before a transitive cross-engine producer
/// finishes its deferred host write or error publication.
const fn same_engine_wait_completed(
    engine: Engine,
    has_outputs: bool,
    query_access: bool,
) -> bool {
    (matches!(engine, Engine::Compute) && query_access)
        || (matches!(engine, Engine::Render) && has_outputs)
}

trait FrontierEntry {
    fn status(&self) -> i32;
    fn ready_is_completed(&self) -> bool;
    fn publication(&self) -> Option<&Arc<Publication>>;
    fn completed(&self) -> &Fence;
    fn scheduled(&self) -> &Fence;
}

impl FrontierEntry for Fences {
    fn status(&self) -> i32 {
        self.status()
    }

    fn ready_is_completed(&self) -> bool {
        self.ready.raw() == self.completed.raw()
    }

    fn publication(&self) -> Option<&Arc<Publication>> {
        self.publication.as_ref()
    }

    fn completed(&self) -> &Fence {
        &self.completed
    }

    fn scheduled(&self) -> &Fence {
        &self.scheduled
    }
}

struct EnqueueOrdering<'a> {
    predecessors: KVec<&'a Arc<Publication>>,
    host_waits: KVec<Fence>,
    firmware_waits: KVec<(Fence, Fence)>,
}

impl<'a> EnqueueOrdering<'a> {
    fn build<E: FrontierEntry>(
        same_frontier: &'a [(u64, E)],
        other_frontier: &'a [(u64, E)],
        order: Order,
        engine: Engine,
        host_waits: bool,
        has_outputs: bool,
        query_access: bool,
    ) -> Result<Self> {
        let index = engine.index();
        let other = 1 - index;

        let same_prefix = order.wait_through[index];
        let same_covered = if same_prefix.is_some() {
            let end =
                same_frontier.partition_point(|(sequence, _)| Order::contains(same_prefix, *sequence));
            &same_frontier[..end]
        } else {
            &[]
        };

        let other_prefix = order.wait_through[other];
        let other_covered = if other_prefix.is_some() {
            let end = other_frontier
                .partition_point(|(sequence, _)| Order::contains(other_prefix, *sequence));
            &other_frontier[..end]
        } else {
            &[]
        };

        let same_wait = same_engine_wait_completed(engine, has_outputs, query_access);
        let same_chain = !same_wait;
        let cross_chain =
            cross_engine_chain_publication(host_waits, has_outputs, engine, query_access);

        // Retain covered failed publications (`status() <= 0`, not only `== 0`)
        // that survived `prune_frontier()`: if a predecessor's deferred error
        // published between pruning and link selection, `add_successor()` still
        // delivers that failure immediately instead of letting the successor
        // signal success.
        let mut predecessors = KVec::new();
        if same_chain {
            for (_, entry) in same_covered {
                if entry.status() <= 0 {
                    if let Some(publication) = entry.publication() {
                        predecessors.push(publication, GFP_KERNEL)?;
                    }
                }
            }
        }
        if cross_chain {
            for (_, entry) in other_covered {
                if entry.status() <= 0 {
                    if let Some(publication) = entry.publication() {
                        predecessors.push(publication, GFP_KERNEL)?;
                    }
                }
            }
        }

        let mut host = KVec::new();
        let mut firmware = KVec::new();
        if same_wait {
            for (_, entry) in same_covered {
                if entry.status() <= 0
                    && (entry.ready_is_completed() || entry.publication().is_some())
                {
                    host.push(entry.completed().clone(), GFP_KERNEL)?;
                }
            }
        }

        for (_, entry) in other_covered {
            // Successful completion includes host timestamp publication. Its
            // ready fence adds no ordering and would prevent shallow inline
            // publication. Failed and pending producers still need checking.
            if entry.status() > 0 {
                continue;
            }
            if cross_engine_wait_completed(
                host_waits,
                has_outputs,
                engine,
                query_access,
                entry.publication().is_some(),
            ) {
                host.push(entry.completed().clone(), GFP_KERNEL)?;
            } else {
                firmware.push(
                    (entry.scheduled().clone(), entry.completed().clone()),
                    GFP_KERNEL,
                )?;
            }
        }

        Ok(Self {
            predecessors,
            host_waits: host,
            firmware_waits: firmware,
        })
    }
}

#[kunit_tests(asahi_g17_cross_engine_timestamps)]
mod tests {
    use super::*;
    use crate::g17::{fence::independent, job::fence_status, status::VmStatus};

    static TEST_KEY: Pin<&LockClassKey> = kernel::static_lock_class!();

    struct TestEntry {
        ready: Fence,
        scheduled: Fence,
        completed: Fence,
        publication: Option<Arc<Publication>>,
    }

    impl FrontierEntry for TestEntry {
        fn status(&self) -> i32 {
            fence_status(&self.completed)
        }

        fn ready_is_completed(&self) -> bool {
            self.ready.raw() == self.completed.raw()
        }

        fn publication(&self) -> Option<&Arc<Publication>> {
            self.publication.as_ref()
        }

        fn completed(&self) -> &Fence {
            &self.completed
        }

        fn scheduled(&self) -> &Fence {
            &self.scheduled
        }
    }

    fn test_entry(
        contexts: &FenceContexts,
        status: &Arc<VmStatus>,
        with_pub: bool,
    ) -> (TestEntry, Option<Arc<Publication>>) {
        let ready = Fence::from_fence(&independent(contexts).unwrap());
        let scheduled = Fence::from_fence(&independent(contexts).unwrap());
        if with_pub {
            let (publication, completed) = Publication::test_pair(contexts, status).unwrap();
            (
                TestEntry {
                    ready,
                    scheduled,
                    completed,
                    publication: Some(publication.clone()),
                },
                Some(publication),
            )
        } else {
            let completed = Fence::from_fence(&independent(contexts).unwrap());
            (
                TestEntry {
                    ready,
                    scheduled,
                    completed,
                    publication: None,
                },
                None,
            )
        }
    }

    #[test]
    fn transitive_render_window_waits_for_intermediate_completion() {
        let contexts = FenceContexts::new(1, c_str!("asahi_q_ts_test"), TEST_KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();

        // Compute A writes a timestamp (has ordered host publication).
        let (comp_a, pub_a) = test_entry(&contexts, &status, true);
        let pub_a = pub_a.unwrap();
        PreparedPublication::new(pub_a.clone(), core::iter::empty())
            .unwrap()
            .activate();
        let compute_frontier = [(1u64, comp_a)];

        // Render B has no timestamps and covers Compute A: overlaps A via
        // firmware wait and chains its publication behind A.
        let order_b = Order {
            sequence: 1,
            wait_through: [None, Some(1)],
            tiling_wait_through: [None, None],
        };
        let plan_b = EnqueueOrdering::build(
            &[] as &[(u64, TestEntry)],
            &compute_frontier,
            order_b,
            Engine::Render,
            false,
            false,
            true,
        )
        .unwrap();
        assert!(plan_b.host_waits.is_empty());
        assert_eq!(plan_b.firmware_waits.len(), 1);
        assert_eq!(plan_b.predecessors.len(), 1);

        let (rend_b, pub_b) = test_entry(&contexts, &status, true);
        let pub_b = pub_b.unwrap();
        let rend_b_completed = rend_b.completed.raw();
        PreparedPublication::new(pub_b.clone(), plan_b.predecessors.iter().copied())
            .unwrap()
            .activate();
        let render_frontier = [(1u64, rend_b)];

        // Render C writes a timestamp (may use fragment-end window) and covers
        // only Render B: must host-wait on B's completion fence so C cannot
        // write the slot or bypass A's error before A publishes through B.
        let order_c = Order {
            sequence: 2,
            wait_through: [Some(1), None],
            tiling_wait_through: [None, None],
        };
        let plan_c = EnqueueOrdering::build(
            &render_frontier,
            &compute_frontier,
            order_c,
            Engine::Render,
            false,
            true,
            true,
        )
        .unwrap();
        assert_eq!(plan_c.host_waits.len(), 1);
        assert_eq!(plan_c.host_waits[0].raw(), rend_b_completed);
        assert!(plan_c.firmware_waits.is_empty());
        assert!(plan_c.predecessors.is_empty());

        pub_a.finish(Ok([1, 2, 0, 0]));
        pub_b.finish(Ok([3, 4, 0, 0]));
    }

    #[test]
    fn predecessor_failing_after_prune_propagates_to_same_engine_successor() {
        let contexts = FenceContexts::new(1, c_str!("asahi_q_fail_test"), TEST_KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();

        let (comp_a, pub_a) = test_entry(&contexts, &status, true);
        let pub_a = pub_a.unwrap();
        PreparedPublication::new(pub_a.clone(), core::iter::empty())
            .unwrap()
            .activate();

        let (rend_b, pub_b) = test_entry(&contexts, &status, true);
        let pub_b = pub_b.unwrap();
        PreparedPublication::new(pub_b.clone(), core::iter::once(&pub_a))
            .unwrap()
            .activate();

        // Render B retires cleanly in hardware while waiting on Compute A, so
        // prune_frontier() sees B pending (`status() == 0`).
        pub_b.finish(Ok([1, 2, 0, 0]));
        assert_eq!(rend_b.status(), 0);

        // Compute A's deferred error publishes B's PRODUCER_FAILED after
        // prune_frontier() and before Render C selects its predecessors.
        pub_a.finish(Err(EIO));
        assert!(rend_b.status() < 0);

        let render_frontier = [(1u64, rend_b)];
        let compute_frontier = [(1u64, comp_a)];
        let order_c = Order {
            sequence: 2,
            wait_through: [Some(1), None],
            tiling_wait_through: [None, None],
        };
        let plan_c = EnqueueOrdering::build(
            &render_frontier,
            &compute_frontier,
            order_c,
            Engine::Render,
            false,
            false,
            true,
        )
        .unwrap();
        assert_eq!(plan_c.predecessors.len(), 1);

        let (pub_c, fence_c) = Publication::test_pair(&contexts, &status).unwrap();
        PreparedPublication::new(pub_c.clone(), plan_c.predecessors.iter().copied())
            .unwrap()
            .activate();
        pub_c.finish(Ok([3, 4, 0, 0]));
        assert!(fence_status(&fence_c) < 0);
    }

    #[test]
    fn shared_timestamp_slot_keeps_host_wait_in_both_directions() {
        let contexts = FenceContexts::new(1, c_str!("asahi_q_dir_test"), TEST_KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();

        let (rend_a, pub_ra) = test_entry(&contexts, &status, true);
        let pub_ra = pub_ra.unwrap();
        PreparedPublication::new(pub_ra.clone(), core::iter::empty())
            .unwrap()
            .activate();
        let rend_a_completed = rend_a.completed.raw();
        let render_frontier = [(1u64, rend_a)];

        // Render(ts) -> NO_QUERY_ACCESS Compute writing timestamps.
        let plan_rc = EnqueueOrdering::build(
            &[] as &[(u64, TestEntry)],
            &render_frontier,
            Order {
                sequence: 1,
                wait_through: [Some(1), None],
                tiling_wait_through: [None, None],
            },
            Engine::Compute,
            false,
            true,
            false,
        )
        .unwrap();
        assert_eq!(plan_rc.host_waits.len(), 1);
        assert_eq!(plan_rc.host_waits[0].raw(), rend_a_completed);
        assert!(plan_rc.firmware_waits.is_empty());

        let (comp_a, pub_ca) = test_entry(&contexts, &status, true);
        let pub_ca = pub_ca.unwrap();
        PreparedPublication::new(pub_ca.clone(), core::iter::empty())
            .unwrap()
            .activate();
        let comp_a_completed = comp_a.completed.raw();
        let compute_frontier = [(1u64, comp_a)];

        // Compute(ts) -> Render writing timestamps (including fragment-end window).
        let plan_cr = EnqueueOrdering::build(
            &[] as &[(u64, TestEntry)],
            &compute_frontier,
            Order {
                sequence: 1,
                wait_through: [None, Some(1)],
                tiling_wait_through: [None, None],
            },
            Engine::Render,
            false,
            true,
            true,
        )
        .unwrap();
        assert_eq!(plan_cr.host_waits.len(), 1);
        assert_eq!(plan_cr.host_waits[0].raw(), comp_a_completed);
        assert!(plan_cr.firmware_waits.is_empty());

        pub_ra.finish(Ok([1, 2, 0, 0]));
        pub_ca.finish(Ok([3, 4, 0, 0]));
    }
}
