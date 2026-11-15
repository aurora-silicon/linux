// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Private render UMA/heap backing and device-wide PB-slot leases.
//!
//! Each VM keeps two persistent UMA/heap units. Each render command installs a fresh
//! PB manager with full InitBM and never reinitializes exposed cursors. The previous
//! PB-slot lease survives until the replacement's complete normal retirement.

#[path = "pools/lifetime.rs"]
pub(crate) mod lifetime;

use super::fw::buffer::{self, uma_manager};
use super::hw::{client, engine, pte};
use super::object::{Class, FwObject, Objects, Placement, Request};
use super::Firmware;
use crate::driver::AsahiDrmDevice;
use core::{
    fmt,
    sync::atomic::{AtomicBool, AtomicU32, Ordering},
};
use kernel::{
    device,
    prelude::*,
    sync::{aref::ARef, Arc},
};

#[derive(Copy, Clone, Default)]
pub(crate) struct AllocatorRefs {
    pub(crate) uma_mgr: [u64; 2],
    /// Firmware aliases of GPU-writable per-engine auxiliary state, separate from the manager.
    pub(crate) uma_aux: [u64; 2],
    pub(crate) uma_gpu: u64,
    pub(crate) pb_mgr: u64,
    pub(crate) scene_list: u64,
    pub(crate) mgr_misc: u64,
    pub(crate) init_blocks: u32,
    pub(crate) parameter_id: u32,
    #[allow(dead_code)]
    pub(crate) submitted_fw: u64,
    #[allow(dead_code)]
    pub(crate) counter_completed_fw: u64,
    pub(crate) uma_submitted: u64,
    pub(crate) init_buffer: bool,
}

struct Allocator {
    descriptor: Arc<FwObject>,
    auxiliary: Arc<FwObject>,
    #[allow(dead_code)]
    pool: Arc<FwObject>,
    #[allow(dead_code)]
    state: Arc<FwObject>,
    #[allow(dead_code)]
    list: Arc<FwObject>,
    submitted: Arc<FwObject>,
    heap: Arc<FwObject>,
    scene_list: Arc<FwObject>,
    published: AtomicBool,
    id: u32,
}

#[derive(Default)]
pub(crate) struct PbStats {
    pub(crate) requests: u64,
    #[allow(dead_code)]
    pub(crate) grown: u64,
    #[allow(dead_code)]
    pub(crate) refused: u64,
    pub(crate) bad: u64,
}

pub(crate) struct PoolSet {
    units: KVec<Allocator>,
}

fn alloc(
    o: &mut Objects,
    size: usize,
    class: Class,
    client: Option<(u64, u64)>,
    cpu: bool,
) -> Result<Arc<FwObject>> {
    o.alloc(Request {
        size,
        class,
        placement: Placement::Anywhere,
        offset: 0,
        client,
        lower: None,
        cpu,
    })
}
fn gva(o: &FwObject) -> Result<u64> {
    o.gva.ok_or(EINVAL)
}
fn read64(o: &FwObject, at: usize) -> Result<u64> {
    // Stable high/low/high observation of an unaligned firmware-owned u64.
    for _ in 0..4 {
        let high = o.r32(at + 4)?;
        let low = o.r32(at)?;
        if high == o.r32(at + 4)? {
            return Ok(u64::from(high) << 32 | u64::from(low));
        }
    }
    Err(EAGAIN)
}
fn write_records<const N: usize>(
    o: &FwObject,
    count: usize,
    record: impl Fn(usize) -> [u8; N],
) -> Result {
    let mut chunk = [0u8; 512];
    let mut first = 0;
    while first < count {
        let n = (count - first).min(512 / N);
        for j in 0..n {
            chunk[j * N..(j + 1) * N].copy_from_slice(&record(first + j));
        }
        o.write(first * N, &chunk[..n * N])?;
        first += n;
    }
    Ok(())
}
impl Allocator {
    fn new(o: &mut Objects, k: u64) -> Result<Self> {
        let base = client::render_base(k);
        let nc = |va| Some((va, pte::LEAF_NC));
        // Native UMA software metadata is ASC-owned and never needs a client GPU alias.
        // In particular its firmware pointers, +0x54 completion and +0x5c exclusion flag
        // must not share a writable GPU page with the engine auxiliary records.
        let descriptor = alloc(o, uma_manager::SIZE, Class::Fw, None, true)?;
        let auxiliary = alloc(
            o,
            0x4000,
            Class::Shared,
            nc(client::render_descriptor(k)),
            false,
        )?;
        let pool = alloc(
            o,
            0x2000000,
            Class::Shared,
            nc(client::render_pool(k)),
            false,
        )?;
        let state = alloc(o, 0x20000, Class::Shared, nc(client::render_state(k)), true)?;
        let list = alloc(o, 0x4000, Class::Shared, nc(client::render_list(k)), true)?;
        let submitted = alloc(o, 0x40, Class::Fw, None, true)?;
        let heap = alloc(
            o,
            client::PB_HEAP_SIZE as usize,
            Class::Shared,
            nc(client::pb_heap(k)),
            false,
        )?;
        let scene_list = alloc(
            o,
            buffer::SCENE_LIST_SIZE,
            Class::Shared,
            nc(base + 0x2070000),
            true,
        )?;
        super::slots::init_uma_pool(&state, &list, gva(&pool)?)?;
        descriptor.write(
            0,
            &buffer::uma_manager_image(
                descriptor.va,
                engine::RENDER_UMA_SLOT,
                gva(&state)?,
                gva(&list)?,
                submitted.va,
            ),
        )?;
        Ok(Self {
            descriptor,
            auxiliary,
            pool,
            state,
            list,
            submitted,
            heap,
            scene_list,
            published: AtomicBool::new(false),
            id: k as u32,
        })
    }
    fn refs(&self) -> Result<AllocatorRefs> {
        let prior = read64(&self.submitted, 0)?;
        if prior & 1 != 0 {
            return Err(EINVAL);
        }
        let next = prior.checked_add(2).ok_or(EOVERFLOW)?;
        Ok(AllocatorRefs {
            uma_mgr: [self.descriptor.va; 2],
            uma_aux: [self.auxiliary.va + 0x100, self.auxiliary.va + 0x200],
            uma_gpu: gva(&self.auxiliary)?,
            pb_mgr: 0,
            scene_list: gva(&self.scene_list)?,
            mgr_misc: 0,
            init_blocks: buffer::MAX_BLOCKS,
            parameter_id: self.id,
            submitted_fw: self.submitted.va,
            counter_completed_fw: self.descriptor.va + 0x54,
            uma_submitted: next,
            init_buffer: true,
        })
    }
    fn retired(&self) -> Result<bool> {
        let submitted = read64(&self.submitted, 0)?;
        let completed = read64(&self.descriptor, uma_manager::COMPLETED)?;
        Ok(submitted != 0 && submitted == completed)
    }
}
impl PoolSet {
    pub(crate) fn new(o: &mut Objects) -> Result<Self> {
        let mut units = KVec::with_capacity(2, GFP_KERNEL)?;
        for k in 0..2 {
            units.push(Allocator::new(o, k)?, GFP_KERNEL)?;
        }
        Ok(Self { units })
    }
    #[allow(dead_code)]
    pub(crate) fn allocator_refs_for(&self, k: usize) -> Result<AllocatorRefs> {
        self.units.get(k).ok_or(EINVAL)?.refs()
    }
    pub(crate) fn prepare(&self, k: usize, _draw: u64) -> Result<AllocatorRefs> {
        let u = self.units.get(k).ok_or(EINVAL)?;
        if u.published.load(Ordering::Acquire) && !u.retired()? {
            return Err(EBUSY);
        }
        // Descriptor pointer/counter and slot must still identify this exact retained unit.
        if read64(&u.descriptor, 0)? != u.descriptor.va
            || u.descriptor.r32(8)? != engine::RENDER_UMA_SLOT
            || read64(&u.descriptor, 0x4c)? != u.submitted.va
        {
            return Err(EINVAL);
        }
        u.refs()
    }
    pub(crate) fn publish(&self, k: usize, submitted: u64) -> Result {
        let u = self.units.get(k).ok_or(EINVAL)?;
        if submitted != read64(&u.submitted, 0)?.checked_add(2).ok_or(EOVERFLOW)? {
            return Err(EINVAL);
        }
        u.submitted.w64(0, submitted)?;
        u.published.store(true, Ordering::Release);
        Ok(())
    }
    pub(crate) fn retired(&self, k: usize) -> Result<bool> {
        self.units.get(k).ok_or(EINVAL)?.retired()
    }
    pub(crate) fn heap_gva(&self, k: usize) -> Result<u64> {
        gva(&self.units.get(k).ok_or(EINVAL)?.heap)
    }
}

/// Device-wide physical PB-slot leases. Replacement requires a complete normal receipt.
pub(crate) struct Pools {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) stats: PbStats,
    owners: [Option<Arc<PbEpoch>>; 2],
}
impl Pools {
    pub(crate) fn new(dev: &AsahiDrmDevice) -> Self {
        Self {
            dev: dev.as_ref().as_ref().into(),
            stats: PbStats::default(),
            owners: [None, None],
        }
    }
    pub(crate) fn retire_binding(&mut self, k: usize, epoch: &Arc<PbEpoch>) -> Result {
        *self.owners.get_mut(k).ok_or(EINVAL)? = Some(epoch.clone());
        Ok(())
    }
    pub(crate) fn write_stats(&self, f: &mut impl fmt::Write) -> fmt::Result {
        writeln!(
            f,
            "pb: M4 private allocators blocks {} requests {} bad {}",
            buffer::MAX_BLOCKS,
            self.stats.requests,
            self.stats.bad
        )
    }
}

/// One never-reinitialized PB manager installation. Even a same-VM command gets a fresh
/// manager and full InitBM; no host reset of exposed firmware cursors is required.
pub(crate) struct PbEpoch(crate::cleanup::Deferred<PbEpochInner>);
impl core::ops::Deref for PbEpoch {
    type Target = PbEpochInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct PbEpochInner {
    pub(crate) manager: Arc<FwObject>,
    pages: Arc<FwObject>,
    blocks: Arc<FwObject>,
    ring: Arc<FwObject>,
    counter: Arc<FwObject>,
    pub(crate) misc: Arc<FwObject>,
    pub(crate) initbm: Arc<FwObject>,
    assigned: AtomicU32,
    exposed: AtomicBool,
    // Unmap private records before releasing the VM's aliases and ASID.
    _objects: Objects,
    _vm: Arc<super::vm::VmState>,
    // Return admission credit only after deferred unmap and binding release.
    _permit: super::budget::Permit,
}
impl PbEpoch {
    pub(crate) fn new(
        factory: &super::object::Factory,
        vm: &Arc<super::vm::VmState>,
        permit: super::budget::Permit,
    ) -> Result<Arc<Self>> {
        let mut o = factory.objects(&vm.vm);
        let records = alloc(&mut o, 0x500, Class::Fw, None, true)?;
        let mut inner = PbEpochInner {
            manager: FwObject::view(&records, 0, 0xc0)?,
            pages: o.alloc_dynamic_client(buffer::PAGE_LIST_SIZE)?,
            blocks: alloc(&mut o, buffer::BLOCK_LIST_SIZE, Class::Fw, None, true)?,
            ring: FwObject::view(&records, 0x100, 8)?,
            counter: FwObject::view(&records, 0x200, 0x40)?,
            misc: FwObject::view(&records, 0x300, 0x40)?,
            initbm: FwObject::view(&records, 0x400, 0x20)?,
            assigned: AtomicU32::new(u32::MAX),
            exposed: AtomicBool::new(false),
            _objects: o,
            _vm: vm.clone(),
            _permit: permit,
        };
        inner._objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(inner)?),
            GFP_KERNEL,
        )?)
    }

    /// Seed only unpublished storage. A retry may choose another physical slot; after
    /// exposure neither the manager nor any of its firmware-mutated lists is reset.
    pub(crate) fn prepare(&self, k: usize, heap: u64) -> Result {
        if k >= 2 || self.exposed.load(Ordering::Acquire) {
            return Err(EINVAL);
        }
        write_records(&self.pages, buffer::MAX_PAGES as usize, |i| {
            buffer::page_record(heap, i as u32).to_le_bytes()
        })?;
        write_records(&self.blocks, buffer::MAX_BLOCKS as usize, |i| {
            u64::from(buffer::block_record(heap, i as u32)).to_le_bytes()
        })?;
        self.ring.w32(0, buffer::MAX_BLOCKS)?;
        self.ring.w32(4, buffer::MAX_BLOCKS)?;
        self.counter.w64(0, 1)?;
        self.manager.write(
            0,
            &buffer::pb_manager_image(
                k as u32,
                self.pages.va,
                gva(&self.pages)?,
                self.blocks.va,
                self.ring.va,
                self.counter.va,
            ),
        )?;
        self.assigned.store(k as u32, Ordering::Release);
        Ok(())
    }
    pub(crate) fn check_assignment(&self, k: usize) -> Result {
        if self.assigned.load(Ordering::Acquire) != k as u32 {
            return Err(EINVAL);
        }
        Ok(())
    }
    pub(crate) fn expose(&self) {
        self.exposed.store(true, Ordering::Release);
    }
}

impl Firmware {
    pub(crate) fn init_render_pools(&mut self, rebuild: bool) -> Result {
        // An abort must quarantine its allocation set. M4 has no qualified allocator reset.
        if rebuild {
            return Err(EIO);
        }
        Ok(())
    }
    pub(crate) fn answer_growth(&mut self) -> Result {
        // M4 publishes its entire bounded heap up front; G15's event-7/device-control
        // protocol is not an M4 ABI. Never synthesize an unqualified firmware reply.
        if !self.events.grow.is_empty() {
            self.pools.stats.requests += self.events.grow.len() as u64;
            self.pools.stats.bad += self.events.grow.len() as u64;
            return Err(super::boot::eproto());
        }
        Ok(())
    }
}
