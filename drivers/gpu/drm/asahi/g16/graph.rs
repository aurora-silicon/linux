// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Retained M4 queue graphs. Pools and descriptors belong to resource units, never queues.
//!
//! A graph is initialized once. The firmware owns its context, queue and notifier records
//! afterwards; only host producer and threshold cells may be changed during publication.

use kernel::{
    device,
    prelude::*,
    sync::{aref::ARef, Arc},
};

use super::channel::Channel;
use super::fw::graph::{self, job_list, notifier_page, qinfo, qstate, sched_ctx};
use super::hw::{pte, sizes, tuning, FwAbi};
use super::object::{Class, Factory, FwObject, Objects, Placement, Request};
use super::Firmware;
use crate::driver::AsahiDrmDevice;

/// Firmware-owned queue graph records initialized once before `register_context()`.
/// Firmware reads and writes them through the ASC-cached mapping after initialization;
/// host receipt checks authenticate on NC `qstate::READ` and submit-pipe cursors.
pub(crate) const fn graph_fw() -> Class {
    Class::FwCached
}
pub(crate) const fn qinfo_cached() -> bool {
    matches!(graph_fw(), Class::FwCached)
}
pub(crate) const fn cmd_fw() -> Class {
    if tuning::FW_CACHED {
        Class::FwCached
    } else {
        Class::Fw
    }
}
pub(crate) const fn cmd_shared() -> Class {
    if tuning::FW_CACHED {
        Class::SharedCached
    } else {
        Class::Shared
    }
}
#[allow(dead_code)]
pub(crate) const fn notifier_class(_bit: u32) -> Class {
    Class::Fw
}
#[allow(dead_code)]
pub(crate) const fn manager_class(class: Class) -> Class {
    class
}
#[allow(dead_code)]
pub(crate) const fn queue_scratch() -> Class {
    graph_fw()
}

/// A private client alias uses the working M4 driver's AttrIndex-2 NC policy.
/// Firmware upper mappings retain their independently selected ASC cache class.
#[derive(Copy, Clone)]
pub(crate) struct Client {
    pub(crate) page: u64,
    pub(crate) leaf: u64,
}
impl Client {
    pub(crate) const fn nc(page: u64) -> Option<Self> {
        Some(Self {
            page,
            leaf: pte::LEAF_NC,
        })
    }
}

pub(crate) fn alloc(
    objects: &mut Objects,
    size: usize,
    class: Class,
    client: Option<Client>,
    offset: usize,
    lower: bool,
) -> Result<Arc<FwObject>> {
    objects.alloc(Request {
        size,
        class,
        placement: Placement::Anywhere,
        offset,
        client: client.map(|c| (c.page, c.leaf)),
        lower: if lower { client.map(|c| c.page) } else { None },
        cpu: true,
    })
}

/// Work lists belong to the kernel GPU task. Firmware prepares them through the upper
/// mapping and hardware reads the existing lower address; applications get no alias.
/// Preserve the established cache classes and wire addresses while separating ownership.
#[allow(dead_code)]
pub(crate) fn alloc_work(
    objects: &mut Objects,
    size: usize,
    lower: u64,
    offset: usize,
) -> Result<Arc<FwObject>> {
    objects.alloc(Request {
        size,
        class: cmd_shared(),
        placement: Placement::Anywhere,
        offset,
        client: None,
        lower: Some(lower),
        cpu: true,
    })
}

pub(crate) struct ComputeGraph {
    pub(crate) sched_ctx: Arc<FwObject>,
    /// Whole 0x1000 page, ASC-cached; compute's record is +0x400. The firmware reads and
    /// updates the record around every completed item and cleans it itself, so it is mapped
    /// like macOS 26B123 maps its compute notifier records (`0x443`); the host writes it only
    /// before the firmware first sees it.
    pub(crate) notifier: Arc<FwObject>,
    pub(crate) threshold: Arc<FwObject>,
    pub(crate) job_list: Arc<FwObject>,
    pub(crate) qinfo: Arc<FwObject>,
    pub(crate) qstate: Arc<FwObject>,
    pub(crate) ring: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) qscratch: Arc<FwObject>,
    /// Uncached service stamp cell the firmware stores each finished command's target into
    /// (work +0x800); the host reads completions here, never from the cached notifier page.
    pub(crate) user_stamp: Arc<FwObject>,
    pub(crate) fw_stamp: Arc<FwObject>,
}

struct QueueSession {
    qinfo: Arc<FwObject>,
    qstate: Arc<FwObject>,
    user_stamp: Arc<FwObject>,
    fw_stamp: Arc<FwObject>,
}
impl QueueSession {
    fn new(o: &mut Objects) -> Result<Self> {
        Ok(Self {
            qinfo: alloc(o, qinfo::SIZE, graph_fw(), None, 0, false)?,
            qstate: alloc(o, qstate::SIZE, Class::Fw, None, 0, false)?,
            user_stamp: alloc(o, 8, Class::Shared, None, 0, false)?,
            fw_stamp: alloc(o, 8, Class::Shared, None, 0, false)?,
        })
    }
    /// Completes the queue with its firmware-cached work ring from a device-lifetime
    /// [`Factory::persistent_objects`] allocator. The host only writes 64-bit work/barrier VAs
    /// into the ring before advancing the uncached `qstate::WRITE` producer and never reads
    /// back firmware writes, matching macOS 26B123's `0x443` command-ring mapping.
    fn finish(self, rings: &mut Objects) -> Result<QueueObjects> {
        Ok(QueueObjects {
            qinfo: self.qinfo,
            qstate: self.qstate,
            ring: alloc(
                rings,
                sizes::QUEUE_CAP as usize * 8,
                Class::FwCoherent,
                None,
                0,
                false,
            )?,
            user_stamp: self.user_stamp,
            fw_stamp: self.fw_stamp,
        })
    }
}

struct QueueObjects {
    qinfo: Arc<FwObject>,
    qstate: Arc<FwObject>,
    ring: Arc<FwObject>,
    user_stamp: Arc<FwObject>,
    fw_stamp: Arc<FwObject>,
}
impl QueueObjects {
    #[allow(dead_code)]
    fn new(o: &mut Objects, rings: &mut Objects) -> Result<Self> {
        QueueSession::new(o)?.finish(rings)
    }
}

fn init_queue(
    abi: FwAbi,
    q: &FwObject,
    state: &FwObject,
    ring: &FwObject,
    list: &FwObject,
    context: &FwObject,
) -> Result {
    q.write(
        0,
        &graph::queue_image_for(abi.qinfo(), state.va, ring.va, list.va, q.va, context.va),
    )?;
    state.w32(qstate::CAP, sizes::QUEUE_CAP)
}

fn init_notifier(page: &FwObject, threshold: &FwObject) -> Result {
    for engine in 0..3 {
        let off = engine * 0x200;
        // A separate uncached threshold is legal. The TA/FS service cells keep their native
        // offsets in the uncached render page; compute's service stamp is its queue's own
        // uncached `user_stamp` cell, because the compute page is ASC-cached.
        page.write(
            off,
            &graph::notifier_image(threshold.va + engine as u64 * 8, 1),
        )?;
    }
    Ok(())
}

struct ComputeGraphSession {
    notifier: Arc<FwObject>,
    threshold: Arc<FwObject>,
    job_list: Arc<FwObject>,
    q: QueueSession,
}
impl ComputeGraphSession {
    fn new(o: &mut Objects) -> Result<Self> {
        Ok(Self {
            notifier: alloc(o, 0x1000, Class::FwCached, None, 0, false)?,
            threshold: alloc(o, 24, Class::Fw, None, 0, false)?,
            job_list: alloc(o, job_list::SIZE, graph_fw(), None, 0, false)?,
            q: QueueSession::new(o)?,
        })
    }
    fn finish(self, rings: &mut Objects, sched_ctx: Arc<FwObject>) -> Result<ComputeGraph> {
        let q = self.q.finish(rings)?;
        Ok(ComputeGraph {
            sched_ctx,
            notifier: self.notifier,
            threshold: self.threshold,
            job_list: self.job_list,
            qscratch: q.qinfo.clone(),
            qinfo: q.qinfo,
            qstate: q.qstate,
            ring: q.ring,
            user_stamp: q.user_stamp,
            fw_stamp: q.fw_stamp,
        })
    }
}

impl ComputeGraph {
    #[allow(dead_code)]
    fn new(o: &mut Objects, rings: &mut Objects, sched_ctx: Arc<FwObject>) -> Result<Self> {
        ComputeGraphSession::new(o)?.finish(rings, sched_ctx)
    }
    fn init(&self, abi: FwAbi, init_ctx: bool) -> Result {
        if init_ctx {
            self.sched_ctx.write(0, &graph::context_image())?;
        }
        init_notifier(&self.notifier, &self.threshold)?;
        self.notifier
            .w64(notifier_page::COMPUTE_RECORD, self.threshold.va)?;
        self.job_list.w64(job_list::SELF, self.job_list.va)?;
        init_queue(
            abi,
            &self.qinfo,
            &self.qstate,
            &self.ring,
            &self.job_list,
            &self.sched_ctx,
        )?;
        self.threshold.w64(0, 1)
    }
}

pub(crate) struct RenderGraph {
    pub(crate) qinfo: [Arc<FwObject>; 2],
    pub(crate) qstate: [Arc<FwObject>; 2],
    pub(crate) ring: [Arc<FwObject>; 2],
    #[allow(dead_code)]
    pub(crate) qscratch: [Arc<FwObject>; 2],
    #[allow(dead_code)]
    pub(crate) user_stamp: [Arc<FwObject>; 2],
    pub(crate) fw_stamp: [Arc<FwObject>; 2],
    pub(crate) sched_ctx: Arc<FwObject>,
    /// Whole page: TA record +0, FS +0x200, service stamps +0x620/+0x630.
    pub(crate) notifier: Arc<FwObject>,
    pub(crate) threshold: Arc<FwObject>,
    pub(crate) job_list: [Arc<FwObject>; 2],
    pub(crate) legacy_empty: Arc<FwObject>,
    pub(crate) registration_barrier: Arc<FwObject>,
}
struct RenderGraphSession {
    ta: QueueSession,
    fs: QueueSession,
    notifier: Arc<FwObject>,
    threshold: Arc<FwObject>,
    job_list: [Arc<FwObject>; 2],
    legacy_empty: Arc<FwObject>,
    registration_barrier: Arc<FwObject>,
}
impl RenderGraphSession {
    fn new(o: &mut Objects) -> Result<Self> {
        Ok(Self {
            ta: QueueSession::new(o)?,
            fs: QueueSession::new(o)?,
            notifier: alloc(o, 0x1000, Class::Fw, None, 0, false)?,
            threshold: alloc(o, 24, Class::Fw, None, 0, false)?,
            job_list: [
                alloc(o, job_list::SIZE, graph_fw(), None, 0, false)?,
                alloc(o, job_list::SIZE, graph_fw(), None, 0, false)?,
            ],
            legacy_empty: alloc(o, 0x40, Class::Shared, None, 0, false)?,
            registration_barrier: alloc(
                o,
                super::fw::work::barrier::SIZE,
                cmd_fw(),
                None,
                0,
                false,
            )?,
        })
    }
    fn finish(self, rings: &mut Objects, sched_ctx: Arc<FwObject>) -> Result<RenderGraph> {
        let [ta, fs] = [self.ta.finish(rings)?, self.fs.finish(rings)?];
        Ok(RenderGraph {
            qscratch: [ta.qinfo.clone(), fs.qinfo.clone()],
            qinfo: [ta.qinfo, fs.qinfo],
            qstate: [ta.qstate, fs.qstate],
            ring: [ta.ring, fs.ring],
            user_stamp: [ta.user_stamp, fs.user_stamp],
            fw_stamp: [ta.fw_stamp, fs.fw_stamp],
            sched_ctx,
            notifier: self.notifier,
            threshold: self.threshold,
            job_list: self.job_list,
            legacy_empty: self.legacy_empty,
            registration_barrier: self.registration_barrier,
        })
    }
}

impl RenderGraph {
    #[allow(dead_code)]
    fn new(o: &mut Objects, rings: &mut Objects, context: Arc<FwObject>) -> Result<Self> {
        RenderGraphSession::new(o)?.finish(rings, context)
    }
    fn init(&self, abi: FwAbi) -> Result {
        init_notifier(&self.notifier, &self.threshold)?;
        for e in 0..2 {
            self.job_list[e].w64(job_list::SELF, self.job_list[e].va)?;
            init_queue(
                abi,
                &self.qinfo[e],
                &self.qstate[e],
                &self.ring[e],
                &self.job_list[e],
                &self.sched_ctx,
            )?;
            self.threshold.w64(e * 8, 2)?;
        }
        Ok(())
    }
}

/// Session-side queue graph allocations before sealing the session allocator and attaching
/// the firmware-cached work rings from a probe-time device-lifetime
/// [`Factory::persistent_objects_probe`] pool.
pub(crate) struct GraphSession {
    compute: KVec<ComputeGraphSession>,
    render: RenderGraphSession,
}

pub(crate) struct Graphs {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) compute: KVec<ComputeGraph>,
    pub(crate) render: RenderGraph,
    /// Owns the firmware-cached work rings and shared scheduling context for the device lifetime
    /// (never dropped).
    _ring_objects: core::mem::ManuallyDrop<Objects>,
}
impl Graphs {
    #[inline(never)]
    pub(crate) fn prepare(o: &mut Objects) -> Result<GraphSession> {
        let mut compute = KVec::with_capacity(sizes::COMPUTE_PIPES, GFP_KERNEL)?;
        for _ in 0..sizes::COMPUTE_PIPES {
            compute.push(ComputeGraphSession::new(o)?, GFP_KERNEL)?;
        }
        let render = RenderGraphSession::new(o)?;
        Ok(GraphSession { compute, render })
    }

    #[inline(never)]
    pub(crate) fn finish(
        dev: &AsahiDrmDevice,
        session: GraphSession,
        factory: &Factory,
    ) -> Result<Self> {
        // Never dropped even on partial failure: ASC-cached mappings have no qualified teardown.
        let mut ring_objects = core::mem::ManuallyDrop::new(factory.persistent_objects_probe());
        // Share one ASC-cached scheduling context across all compute pipes and the render graph,
        // matching macOS 26B123 (`0x443`). `register_context()` syncs the cacheable host view
        // before polling registration once at boot; firmware reads and writes it on every kick
        // and completion.
        let sched_ctx = alloc(
            &mut ring_objects,
            sched_ctx::SIZE,
            Class::FwCoherent,
            None,
            0,
            false,
        )?;
        let mut compute = KVec::with_capacity(sizes::COMPUTE_PIPES, GFP_KERNEL)?;
        for c in session.compute {
            compute.push(c.finish(&mut ring_objects, sched_ctx.clone())?, GFP_KERNEL)?;
        }
        let render = session.render.finish(&mut ring_objects, sched_ctx)?;
        ring_objects.finish_private()?;
        Ok(Self {
            dev: dev.as_ref().as_ref().into(),
            compute,
            render,
            _ring_objects: ring_objects,
        })
    }
}
impl Firmware {
    #[inline(never)]
    pub(crate) fn init_graphs(&mut self) -> Result {
        for ch in [Channel::Ta, Channel::Fs, Channel::Compute] {
            if self.channels.read(ch)? != 0 || self.channels.write_cell(ch)? != 0 {
                return Err(Error::from_errno(-(kernel::bindings::EPROTO as i32)));
            }
        }
        for (pipe, compute) in self.graph.compute.iter().enumerate() {
            compute.init(self.init.abi, pipe == 0)?;
        }
        self.graph.render.init(self.init.abi)?;
        self.slots.init()?;
        self.init_render_pools(false)?;
        super::engine::wmb();
        super::uat::tlbi_all();
        self.register_context()?;
        dev_info!(
            self.graph.dev.as_ref(),
            "G16: M4 queue/resource graphs initialized\n"
        );
        Ok(())
    }
}
