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
use super::hw::{pte, sizes, tuning};
use super::object::{Class, FwObject, Objects, Placement, Request};
use super::Firmware;
use crate::driver::AsahiDrmDevice;

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
    cmd_fw()
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
    /// Whole 0x1000 page; compute's record is +0x400, service stamps +0x640/+0x648.
    pub(crate) notifier: Arc<FwObject>,
    pub(crate) threshold: Arc<FwObject>,
    pub(crate) job_list: Arc<FwObject>,
    pub(crate) qinfo: Arc<FwObject>,
    pub(crate) qstate: Arc<FwObject>,
    pub(crate) ring: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) qscratch: Arc<FwObject>,
    /// Separate uncached engine-stage cells; service stamps are in the notifier page.
    #[allow(dead_code)]
    pub(crate) user_stamp: Arc<FwObject>,
    pub(crate) fw_stamp: Arc<FwObject>,
}

struct QueueObjects {
    qinfo: Arc<FwObject>,
    qstate: Arc<FwObject>,
    ring: Arc<FwObject>,
    user_stamp: Arc<FwObject>,
    fw_stamp: Arc<FwObject>,
}
impl QueueObjects {
    fn new(o: &mut Objects) -> Result<Self> {
        Ok(Self {
            qinfo: alloc(o, qinfo::SIZE, cmd_fw(), None, 0, false)?,
            qstate: alloc(o, qstate::SIZE, Class::Fw, None, 0, false)?,
            ring: alloc(o, sizes::QUEUE_CAP as usize * 8, cmd_fw(), None, 0, false)?,
            user_stamp: alloc(o, 8, Class::Shared, None, 0, false)?,
            fw_stamp: alloc(o, 8, Class::Shared, None, 0, false)?,
        })
    }
}

fn init_queue(
    q: &FwObject,
    state: &FwObject,
    ring: &FwObject,
    list: &FwObject,
    context: &FwObject,
) -> Result {
    q.write(
        0,
        &graph::queue_image(state.va, ring.va, list.va, q.va, context.va),
    )?;
    state.w32(qstate::CAP, sizes::QUEUE_CAP)
}

fn init_notifier(page: &FwObject, threshold: &FwObject) -> Result {
    for engine in 0..3 {
        let off = engine * 0x200;
        // A separate uncached threshold is legal; the service cells retain the native page
        // offsets so completion never depends on cacheable work memory.
        page.write(
            off,
            &graph::notifier_image(threshold.va + engine as u64 * 8, 1),
        )?;
    }
    Ok(())
}

impl ComputeGraph {
    fn new(o: &mut Objects) -> Result<Self> {
        let q = QueueObjects::new(o)?;
        Ok(Self {
            sched_ctx: alloc(o, sched_ctx::SIZE, cmd_fw(), None, 0, false)?,
            notifier: alloc(o, 0x1000, Class::Fw, None, 0, false)?,
            threshold: alloc(o, 24, Class::Fw, None, 0, false)?,
            job_list: alloc(o, job_list::SIZE, cmd_fw(), None, 0, false)?,
            qscratch: q.qinfo.clone(),
            qinfo: q.qinfo,
            qstate: q.qstate,
            ring: q.ring,
            user_stamp: q.user_stamp,
            fw_stamp: q.fw_stamp,
        })
    }
    fn init(&self) -> Result {
        self.sched_ctx.write(0, &graph::context_image())?;
        init_notifier(&self.notifier, &self.threshold)?;
        self.notifier
            .w64(notifier_page::COMPUTE_RECORD, self.threshold.va)?;
        self.job_list.w64(job_list::SELF, self.job_list.va)?;
        init_queue(
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
impl RenderGraph {
    fn new(o: &mut Objects, context: Arc<FwObject>) -> Result<Self> {
        let [ta, fs] = [QueueObjects::new(o)?, QueueObjects::new(o)?];
        Ok(Self {
            qscratch: [ta.qinfo.clone(), fs.qinfo.clone()],
            qinfo: [ta.qinfo, fs.qinfo],
            qstate: [ta.qstate, fs.qstate],
            ring: [ta.ring, fs.ring],
            user_stamp: [ta.user_stamp, fs.user_stamp],
            fw_stamp: [ta.fw_stamp, fs.fw_stamp],
            sched_ctx: context,
            notifier: alloc(o, 0x1000, Class::Fw, None, 0, false)?,
            threshold: alloc(o, 24, Class::Fw, None, 0, false)?,
            job_list: [
                alloc(o, job_list::SIZE, cmd_fw(), None, 0, false)?,
                alloc(o, job_list::SIZE, cmd_fw(), None, 0, false)?,
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
    fn init(&self) -> Result {
        init_notifier(&self.notifier, &self.threshold)?;
        for e in 0..2 {
            self.job_list[e].w64(job_list::SELF, self.job_list[e].va)?;
            init_queue(
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

pub(crate) struct Graphs {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) compute: ComputeGraph,
    pub(crate) render: RenderGraph,
}
impl Graphs {
    pub(crate) fn new(dev: &AsahiDrmDevice, o: &mut Objects) -> Result<Self> {
        let compute = ComputeGraph::new(o)?;
        let render = RenderGraph::new(o, compute.sched_ctx.clone())?;
        Ok(Self {
            dev: dev.as_ref().as_ref().into(),
            compute,
            render,
        })
    }
}
impl Firmware {
    pub(crate) fn init_graphs(&mut self) -> Result {
        for ch in [Channel::Ta, Channel::Fs, Channel::Compute] {
            if self.channels.read(ch)? != 0 || self.channels.write_cell(ch)? != 0 {
                return Err(Error::from_errno(-(kernel::bindings::EPROTO as i32)));
            }
        }
        self.graph.compute.init()?;
        self.graph.render.init()?;
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
