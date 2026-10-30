// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Firmware objects and their cache classes.
//!
//! Each object is a GEM kernel object (write-combined CPU mapping) mapped into the host window
//! at a fixed VA or in [`super::hw::va::FW_ALLOC`] (a bump allocator with an unmapped guard page
//! after each object), with these optional aliases of the same pages:
//!
//! - a context-0 lower alias (`Uat::kernel_lower_vm`, [`super::hw::pte::LOWER`]);
//! - a client alias at a fixed GVA in its owning VM. Private arenas map it directly;
//!   session initdata has no application aliases.
//!
//! The backing is populated and DMA-mapped before any CPU view exists: on this non-DMA-coherent
//! device the DMA mapping cleans the freshly zeroed pages' cacheable alias to the point of
//! coherency, so no dirty line can later overwrite what the host writes through the
//! write-combined view. The firmware-side leaf word is chosen by [`Class`] and must be exact
//! ([`Objects::self_check`] proves the shared page-table code writes it). Host accesses
//! after publication use single, aligned volatile accesses (`r32`/`w32`/`w64`). Objects are
//! retained for their ownership epoch. Session initdata and uncertain owners stay until
//! reboot; private NC arenas can unmap after full retirement and distinct displacement.

use core::ptr::NonNull;

use kernel::{
    new_mutex,
    prelude::*,
    sync::aref::ARef,
    sync::{Arc, Mutex, UniqueArc},
};

use super::hw::{self, pte};
use super::regs::{rmb, wmb};
use super::uat;
use crate::driver::AsahiDrmDevice;
use crate::{gem, mmu};

/// Firmware-side cache class of an object (the leaf words of [`super::hw::pte`]).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Class {
    /// Firmware-only, AttrIndex 2.
    Fw,
    /// Firmware-only, ASC-cached (AttrIndex 0). Never for rings or anything the host writes
    /// after the firmware first touched it.
    FwCached,
    /// Firmware and GPU, AttrIndex 2.
    Shared,
    /// Firmware and GPU, ASC-cached.
    SharedCached,
    /// Firmware device mapping.
    #[allow(dead_code)]
    Mmio,
}

impl Class {
    /// The exact firmware leaf word of this class.
    pub(crate) const fn pte(self) -> u64 {
        match self {
            Class::Fw => pte::FW,
            Class::FwCached => pte::FW_CACHED,
            Class::Shared => pte::SHARED,
            Class::SharedCached => pte::SHARED_CACHED,
            Class::Mmio => pte::MMIO,
        }
    }
}

/// Where an object is placed in the host window.
pub(crate) enum Placement {
    /// At this firmware VA (canonical initdata objects). The payload starts at this VA; the
    /// object starts at its page.
    #[allow(dead_code)]
    At(u64),
    /// Anywhere in the firmware allocator window, followed by an unmapped guard page.
    Anywhere,
}

/// A firmware object.
///
/// # Invariants
///
/// `cpu`, if set, points to a live write-combined kernel mapping of the whole backing (`bytes`
/// bytes), kept alive by `backing`.
pub(crate) struct FwObject {
    /// Firmware (host-window) VA of byte 0 of the payload.
    pub(crate) va: u64,
    /// Client GVA of byte 0, if aliased into every user VM.
    pub(crate) gva: Option<u64>,
    /// Kernel GPU task lower address of byte 0, independent of any application alias.
    pub(crate) lower_gva: Option<u64>,
    /// Payload size.
    pub(crate) size: usize,
    /// Payload offset inside the first page.
    offset: usize,
    /// Mapped bytes: the payload rounded up to whole pages.
    bytes: usize,
    /// Client alias leaf word.
    client_leaf: u64,
    cpu: Option<NonNull<u8>>,
    backing: gem::ObjectRef,
    // Subrecords retain the owning WC vmap as well as their GEM.
    _parent: Option<Arc<FwObject>>,
    limit: Option<usize>,
}

// SAFETY: The object only refers to kernel memory shared with the firmware; every host access
// goes through raw pointers with volatile or byte-copy semantics, serialized by the device lock
// where the protocol needs it.
unsafe impl Send for FwObject {}
// SAFETY: See `Send`.
unsafe impl Sync for FwObject {}

impl FwObject {
    /// Pointer to payload byte `offset` for `len` bytes, bounded by the mapped pages.
    fn ptr(&self, offset: usize, len: usize) -> Result<*mut u8> {
        let cpu = self.cpu.ok_or(EINVAL)?;
        let at = self.offset.checked_add(offset).ok_or(EINVAL)?;
        if self
            .limit
            .is_some_and(|limit| offset.checked_add(len).is_none_or(|end| end > limit))
            || at.checked_add(len).ok_or(EINVAL)? > self.bytes
        {
            return Err(EINVAL);
        }
        // SAFETY: By the type invariant `cpu` maps `bytes` bytes; `at + len` was checked.
        Ok(unsafe { cpu.as_ptr().add(at) })
    }

    /// An owned, bounded firmware-only record inside one NC backing page.
    pub(crate) fn view(object: &Arc<Self>, offset: usize, size: usize) -> Result<Arc<Self>> {
        object.ptr(offset, size)?;
        if offset.checked_add(size).is_none_or(|end| end > object.size)
            || size == 0
            || object.gva.is_some()
            || object.lower_gva.is_some()
        {
            return Err(EINVAL);
        }
        Ok(Arc::new(
            Self {
                va: object.va + offset as u64,
                gva: None,
                lower_gva: None,
                size,
                offset: object.offset + offset,
                bytes: object.bytes,
                client_leaf: object.client_leaf,
                cpu: object.cpu,
                backing: gem::ObjectRef::new(object.gem()),
                _parent: Some(object.clone()),
                limit: Some(size),
            },
            GFP_KERNEL,
        )?)
    }

    /// Copies bytes into the object before it is published, or into host-owned words.
    pub(crate) fn write(&self, offset: usize, bytes: &[u8]) -> Result {
        let p = self.ptr(offset, bytes.len())?;
        // SAFETY: `p` is valid for `bytes.len()` bytes of the object's mapping, which no Rust
        // reference aliases.
        unsafe { core::ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len()) };
        Ok(())
    }

    /// Fills bytes.
    pub(crate) fn fill(&self, offset: usize, value: u8, len: usize) -> Result {
        let p = self.ptr(offset, len)?;
        // SAFETY: As for `write`.
        unsafe { core::ptr::write_bytes(p, value, len) };
        Ok(())
    }

    /// 32-bit store: a single aligned access, or bytes at an unaligned offset.
    pub(crate) fn w32(&self, offset: usize, value: u32) -> Result {
        let p = self.ptr(offset, 4)?;
        if p as usize % 4 == 0 {
            // SAFETY: `p` is valid and aligned for a u32.
            unsafe { p.cast::<u32>().write_volatile(value.to_le()) };
            Ok(())
        } else {
            self.write(offset, &value.to_le_bytes())
        }
    }

    /// 64-bit store: a single aligned access, or bytes at an unaligned offset.
    pub(crate) fn w64(&self, offset: usize, value: u64) -> Result {
        let p = self.ptr(offset, 8)?;
        if p as usize % 8 == 0 {
            // SAFETY: `p` is valid and aligned for a u64.
            unsafe { p.cast::<u64>().write_volatile(value.to_le()) };
            Ok(())
        } else {
            self.write(offset, &value.to_le_bytes())
        }
    }

    /// 32-bit load of a firmware- or GPU-written word: a single aligned access, or an unaligned
    /// load (words of the unaligned canonical objects).
    pub(crate) fn r32(&self, offset: usize) -> Result<u32> {
        let p = self.ptr(offset, 4)?;
        // SAFETY: `p` is valid for 4 bytes; Normal non-cacheable memory permits an unaligned
        // load.
        Ok(u32::from_le(unsafe {
            if p as usize % 4 == 0 {
                (p as *const u32).read_volatile()
            } else {
                (p as *const u32).read_unaligned()
            }
        }))
    }

    /// Reads firmware-shared bytes without creating a Rust reference to mutable memory.
    /// The caller owns the protocol epoch (for example, a retired descriptor being reused).
    pub(crate) fn read(&self, offset: usize, out: &mut [u8]) -> Result {
        let p = self.ptr(offset, out.len())?;
        rmb();
        for (index, byte) in out.iter_mut().enumerate() {
            // SAFETY: ptr() bounds the complete range; WC memory permits byte accesses.
            *byte = unsafe { p.add(index).read_volatile() };
        }
        rmb();
        Ok(())
    }

    /// A stable 64-bit receipt, including unaligned packed firmware descriptors. Two equal
    /// snapshots separated by a read barrier avoid accepting a torn word. An actively
    /// changing receipt asks the poller to retry instead of fabricating completion.
    pub(crate) fn r64(&self, offset: usize) -> Result<u64> {
        let p = self.ptr(offset, 8)?;
        if p as usize % 8 == 0 {
            rmb();
            // SAFETY: ptr() bounds this naturally aligned 64-bit volatile receipt.
            return Ok(u64::from_le(unsafe { (p as *const u64).read_volatile() }));
        }
        let mut first = [0u8; 8];
        let mut next = [0u8; 8];
        self.read(offset, &mut first)?;
        for _ in 0..3 {
            self.read(offset, &mut next)?;
            if first == next {
                return Ok(u64::from_le_bytes(next));
            }
            first = next;
        }
        Err(EAGAIN)
    }

    /// Mapped bytes (the payload offset plus size, rounded up to pages).
    pub(crate) fn bytes(&self) -> usize {
        self.bytes
    }

    /// The client alias: GVA of the first page and its leaf word. `vm.rs` maps [`Self::gem`]
    /// there in every user VM.
    pub(crate) fn client(&self) -> Option<(u64, u64)> {
        self.gva
            .map(|gva| (gva - self.offset as u64, self.client_leaf))
    }

    /// The backing GEM object, for aliases in other address spaces.
    pub(crate) fn gem(&self) -> ARef<gem::Object> {
        self.backing.gem.clone()
    }
}

/// Allocation request.
pub(crate) struct Request {
    pub(crate) size: usize,
    pub(crate) class: Class,
    pub(crate) placement: Placement,
    /// Payload offset inside the first page (e.g. work images at +0x3760). Ignored
    /// for [`Placement::At`], whose VA gives it.
    pub(crate) offset: usize,
    /// Client alias GVA of the page holding the payload, and its leaf word
    /// ([`pte::LEAF_NC`], [`pte::LEAF_CACHED`] or [`pte::LEAF_PB_LIST`]).
    pub(crate) client: Option<(u64, u64)>,
    /// Context-0 lower alias GVA of the first page, independent of an application alias.
    pub(crate) lower: Option<u64>,
    /// GPU-only objects need no CPU mapping (zeroed at allocation).
    pub(crate) cpu: bool,
}

/// Private allocation coordinates. Only normal non-cacheable objects may be reclaimed.
struct Private {
    vm: mmu::Vm,
    firmware: core::ops::Range<u64>,
    pending: Option<core::ops::Range<u64>>,
}

/// Immutable factory shared by ioctl-side private arena construction.
#[derive(Clone)]
pub(crate) struct Factory {
    dev: ARef<AsahiDrmDevice>,
    uat: Arc<mmu::Uat>,
    window: Arc<Mutex<uat::Window>>,
    firmware: core::ops::Range<u64>,
}

impl Factory {
    pub(crate) fn objects(&self, vm: &mmu::Vm) -> Objects {
        Objects {
            dev: self.dev.clone(),
            uat: self.uat.clone(),
            window: self.window.clone(),
            private: Some(Private {
                vm: vm.clone(),
                firmware: self.firmware.clone(),
                pending: None,
            }),
            sealed: false,
            fw_next: self.firmware.start,
            retained: KVec::new(),
            clients: KVec::new(),
            mappings: KVec::new(),
            error: None,
            pages: 0,
        }
    }
}

/// The firmware object allocator and the set of retained objects.
pub(crate) struct Objects {
    dev: ARef<AsahiDrmDevice>,
    uat: Arc<mmu::Uat>,
    /// The host window: shared L2 mirror and PPL handoff.
    pub(crate) window: Arc<Mutex<uat::Window>>,
    private: Option<Private>,
    sealed: bool,
    /// Next free VA of the firmware allocator window.
    fw_next: u64,
    /// Every object owned by this session or private arena.
    retained: KVec<Arc<FwObject>>,
    /// Objects aliased into every user VM.
    clients: KVec<Arc<FwObject>>,
    /// Every firmware, lower, device and self-check alias owned by this arena. Mapping
    /// ownership is independent of an object handle so a partial construction cannot drop
    /// reachable backing or aliases.
    mappings: KVec<mmu::KernelMapping>,
    /// A partially published allocation is retained and makes further publication fail-stop.
    error: Option<Error>,
    /// Host-window pages backed by objects.
    pages: usize,
}

impl Objects {
    pub(crate) fn new(
        dev: &AsahiDrmDevice,
        uat: Arc<mmu::Uat>,
        window: uat::Window,
    ) -> Result<Self> {
        Ok(Self {
            dev: dev.into(),
            uat,
            window: Arc::pin_init(new_mutex!(window, "g16::Objects::window"), GFP_KERNEL)?,
            private: None,
            sealed: false,
            fw_next: hw::va::FW_ALLOC.start,
            retained: KVec::new(),
            clients: KVec::new(),
            mappings: KVec::new(),
            error: None,
            pages: 0,
        })
    }

    /// Freeze the session allocation boundary before making any private arenas.
    pub(crate) fn private_factory(&mut self) -> Factory {
        self.sealed = true;
        Factory {
            dev: self.dev.clone(),
            uat: self.uat.clone(),
            window: self.window.clone(),
            firmware: self.fw_next..hw::va::FW_ALLOC.end,
        }
    }

    /// Maps private backing into one client and unique kernel/firmware apertures. Mapping
    /// owners retain the GEM independently, including every partial construction failure.
    /// All reclaimable objects are NC from creation; there is no ASC cache transition.
    fn alloc_private(&mut self, req: Request) -> Result<Arc<FwObject>> {
        self.alloc_private_in(req, None)
    }

    pub(crate) fn alloc_dynamic_client(&mut self, size: usize) -> Result<Arc<FwObject>> {
        self.alloc_private_in(
            Request {
                size,
                class: Class::Shared,
                placement: Placement::Anywhere,
                offset: 0,
                client: None,
                lower: None,
                cpu: true,
            },
            Some(0x7040000000..0x7080000000),
        )
    }

    fn alloc_private_in(
        &mut self,
        req: Request,
        client_range: Option<core::ops::Range<u64>>,
    ) -> Result<Arc<FwObject>> {
        if !matches!(req.placement, Placement::Anywhere)
            || !matches!(req.class, Class::Fw | Class::Shared)
            || req.size == 0
            || req.offset >= hw::PAGE as usize
        {
            return Err(EINVAL);
        }
        let private = self.private.as_ref().ok_or(EINVAL)?;
        let bytes = req
            .offset
            .checked_add(req.size)
            .and_then(|size| size.checked_add(hw::PAGE as usize - 1))
            .ok_or(EOVERFLOW)?
            & !(hw::PAGE as usize - 1);
        self.retained.reserve(1, GFP_KERNEL)?;
        self.mappings.reserve(3, GFP_KERNEL)?;
        let mut backing = gem::new_kernel_object_wc(&self.dev, bytes)?;
        let registered = self.dev.registration_guard().ok_or(ENODEV)?;
        let parent: &kernel::platform::Device<kernel::device::Bound> = registered.as_ref();
        gem::prepare_new_object(&backing.gem, parent.as_ref())?;
        let cpu = if req.cpu {
            let vmap = backing.vmap()?;
            let pointer = NonNull::new(vmap.addr.cast::<u8>()).ok_or(ENOMEM)?;
            // SAFETY: Fresh private WC mapping spans bytes and has not been exposed.
            unsafe { core::ptr::write_bytes(pointer.as_ptr(), 0, bytes) };
            Some(pointer)
        } else {
            None
        };
        let firmware = self.uat.kernel_vm().map_in_range(
            &backing.gem,
            0..bytes,
            hw::PAGE,
            private.firmware.clone(),
            mmu::Prot::from_pte(req.class.pte()),
            true,
        )?;
        let va = firmware.iova();
        self.mappings.push(firmware, GFP_KERNEL)?;
        let lower_gva = if req.lower.is_some() {
            // No two live VMs share a context-0 work address. The self-check at 0x70ff...
            // and all fixed client aliases are outside this aperture.
            let lower = self.uat.kernel_lower_vm().map_in_range(
                &backing.gem,
                0..bytes,
                hw::PAGE,
                0x70f1000000..0x70ff000000,
                mmu::Prot::from_pte(pte::LOWER),
                true,
            )?;
            let address = lower.iova() + req.offset as u64;
            self.mappings.push(lower, GFP_KERNEL)?;
            uat::clean_lower(&self.uat)?;
            Some(address)
        } else {
            None
        };
        let client_gva = if let Some(range) = client_range {
            let mapping = private.vm.map_in_range(
                &backing.gem,
                0..bytes,
                hw::PAGE,
                range,
                mmu::Prot::from_pte(pte::LEAF_NC),
                true,
            )?;
            let address = mapping.iova();
            self.mappings.push(mapping, GFP_KERNEL)?;
            Some(address)
        } else if let Some((gva, leaf)) = req.client {
            let end = gva.checked_add(bytes as u64).ok_or(EOVERFLOW)?;
            if gva < hw::client::CLIENT_APERTURE.start
                || end > hw::client::CLIENT_APERTURE.end
                || gva & (hw::PAGE - 1) != 0
                || leaf != pte::LEAF_NC && leaf != pte::LEAF_PB_LIST
            {
                return Err(EINVAL);
            }
            self.mappings.push(
                private
                    .vm
                    .map_at(gva, bytes, &backing.gem, mmu::Prot::from_pte(leaf), false)?,
                GFP_KERNEL,
            )?;
            Some(gva)
        } else {
            None
        };
        wmb();
        let private = self.private.as_mut().ok_or(EINVAL)?;
        let end = va + bytes as u64;
        private.pending = Some(match private.pending.take() {
            Some(range) => range.start.min(va)..range.end.max(end),
            None => va..end,
        });
        let object = Arc::new(
            FwObject {
                va: va + req.offset as u64,
                gva: client_gva.map(|gva| gva + req.offset as u64),
                lower_gva,
                size: req.size,
                offset: req.offset,
                bytes,
                client_leaf: req.client.map_or(pte::LEAF_NC, |(_, leaf)| leaf),
                cpu,
                backing,
                _parent: None,
                limit: None,
            },
            GFP_KERNEL,
        )?;
        self.retained.push(object.clone(), GFP_KERNEL)?;
        self.pages += bytes / hw::PAGE as usize;
        Ok(object)
    }

    /// Publish a completed private arena in one shared-L2 transaction.
    pub(crate) fn finish_private(&mut self) -> Result {
        if let Some(range) = self.private.as_mut().ok_or(EINVAL)?.pending.take() {
            self.window.lock().sync_range(&self.uat, range)?;
        }
        Ok(())
    }

    /// Objects aliased into every user VM, in allocation order.
    pub(crate) fn clients(&self) -> &[Arc<FwObject>] {
        &self.clients
    }

    /// Host-window pages backed by firmware objects.
    #[allow(dead_code)]
    pub(crate) fn pages(&self) -> usize {
        self.pages
    }

    /// Bounded live UAT ownership checks; no user leaf walk and no firmware memory repair.
    pub(crate) fn health(&mut self) -> Result {
        if let Some(error) = self.error {
            return Err(error);
        }
        self.window.lock().check()?;
        self.uat.check_adopted_bindings()
    }

    /// Allocates and maps a zeroed firmware object.
    pub(crate) fn alloc(&mut self, req: Request) -> Result<Arc<FwObject>> {
        self.alloc_split(req, None)
    }

    /// Like [`Self::alloc`], with the first `cached` mapped bytes (a whole number of pages) in
    /// `class` and the rest in the request's class (HardwareData).
    pub(super) fn alloc_split(
        &mut self,
        req: Request,
        prefix: Option<(usize, Class)>,
    ) -> Result<Arc<FwObject>> {
        if let Some(error) = self.error {
            return Err(error);
        }
        if self.private.is_some() {
            if prefix.is_some() {
                return Err(EINVAL);
            }
            return self.alloc_private(req);
        }
        if self.sealed {
            return Err(EBUSY);
        }
        let page = hw::PAGE as usize;
        let (page_va, offset) = match req.placement {
            Placement::At(va) => (va & !(hw::PAGE - 1), (va & (hw::PAGE - 1)) as usize),
            Placement::Anywhere => (self.fw_next, req.offset),
        };
        if req.size == 0 || offset >= page {
            return Err(EINVAL);
        }
        let bytes = offset
            .checked_add(req.size)
            .and_then(|value| value.checked_add(page - 1))
            .ok_or(EOVERFLOW)?
            & !(page - 1);
        let mapped_end = page_va.checked_add(bytes as u64).ok_or(EOVERFLOW)?;
        if page_va < hw::HOST || mapped_end > hw::HOST + hw::HOST_SIZE {
            return Err(EINVAL);
        }
        if let Placement::Anywhere = req.placement {
            if mapped_end.checked_add(hw::PAGE).ok_or(EOVERFLOW)? > hw::va::FW_ALLOC.end {
                return Err(ENOSPC);
            }
        }
        if let Some((gpage, _)) = req.client {
            if gpage & (hw::PAGE - 1) != 0
                || gpage.checked_add(bytes as u64).ok_or(EOVERFLOW)? > hw::USER_RANGE.end
            {
                return Err(EINVAL);
            }
            if self.clients.len() == hw::sizes::MAX_CLIENT {
                return Err(ENOSPC);
            }
        }
        if let Some((cached, _)) = prefix {
            if cached == 0 || cached % page != 0 || cached >= bytes {
                return Err(EINVAL);
            }
        }

        // Allocate every host owner before a mapping can become reachable. In particular no
        // Arc/vector allocation may fail after the live shared-L2 transaction has begun.
        self.retained.reserve(1, GFP_KERNEL)?;
        self.mappings.reserve(3, GFP_KERNEL)?;
        if req.client.is_some() {
            self.clients.reserve(1, GFP_KERNEL)?;
        }
        let object_storage = UniqueArc::<FwObject>::new_uninit(GFP_KERNEL)?;

        // Populate and DMA-map first: this cleans the zeroed pages' cacheable alias to the
        // point of coherency before any write-combined access.
        let mut backing = gem::new_kernel_object_wc(&self.dev, bytes)?;
        // SAFETY: allocation runs during platform probe or under the bound G16
        // owner's admission lock. Its session and supplier remain retained.
        let bound = unsafe { self.dev.as_ref().as_ref().as_bound() };
        let sgt = gem::held_sg_table(&backing.gem, bound)?;
        for range in sgt.iter() {
            let pa = range.dma_address();
            let len = range.dma_len();
            // EPERM for a page the UAT cannot name.
            if pa == 0
                || (pa | len) & (hw::PAGE - 1) != 0
                || (pa + len - 1) & !(hw::PA_MASK | (hw::PAGE - 1)) != 0
            {
                return Err(EPERM);
            }
        }
        let cpu = if req.cpu {
            let vmap = backing.vmap()?;
            let ptr = NonNull::new(vmap.addr.cast::<u8>()).ok_or(ENOMEM)?;
            // SAFETY: this fresh private WC mapping spans bytes and is unpublished.
            unsafe { core::ptr::write_bytes(ptr.as_ptr(), 0, bytes) };
            Some(ptr)
        } else {
            None
        };

        // INVARIANT: cpu is an owned WC vmap of backing. Retain the physical owner before
        // ANY map call: the RAM L3 may already be named by the live reserved shared-L2.
        let object: Arc<FwObject> = object_storage
            .write(FwObject {
                va: page_va + offset as u64,
                gva: req.client.map(|(gpage, _)| gpage + offset as u64),
                lower_gva: req.lower.map(|gpage| gpage + offset as u64),
                size: req.size,
                offset,
                bytes,
                client_leaf: req.client.map_or(pte::LEAF_NC, |(_, leaf)| leaf),
                cpu,
                backing,
                _parent: None,
                limit: None,
            })
            .into();
        self.retained.push(object.clone(), GFP_KERNEL)?;
        self.pages += bytes / page;
        if let Placement::Anywhere = req.placement {
            // A partial failure retains its reservation too: never reuse a potentially cached
            // address while this firmware instance is running.
            self.fw_next += bytes as u64 + hw::PAGE;
        }
        let kernel_vm = self.uat.kernel_vm();
        let guard = matches!(req.placement, Placement::Anywhere);
        let mapped = (|| -> Result {
            match prefix {
                None => {
                    self.mappings.push(
                        kernel_vm.map_at(
                            page_va,
                            bytes,
                            &object.gem(),
                            mmu::Prot::from_pte(req.class.pte()),
                            guard,
                        )?,
                        GFP_KERNEL,
                    )?;
                }
                Some((cached, class)) => {
                    let first = kernel_vm.map_in_range(
                        &object.backing.gem,
                        0..cached,
                        hw::PAGE,
                        page_va..page_va + cached as u64,
                        mmu::Prot::from_pte(class.pte()),
                        false,
                    )?;
                    let first_va = first.iova();
                    self.mappings.push(first, GFP_KERNEL)?;
                    let rest = page_va + cached as u64;
                    let second = kernel_vm.map_in_range(
                        &object.backing.gem,
                        cached..bytes,
                        hw::PAGE,
                        rest..page_va + bytes as u64,
                        mmu::Prot::from_pte(req.class.pte()),
                        guard,
                    )?;
                    let second_va = second.iova();
                    self.mappings.push(second, GFP_KERNEL)?;
                    if first_va != page_va || second_va != rest {
                        return Err(EINVAL);
                    }
                }
            }
            if let Some(lower) = req.lower {
                if lower & (hw::PAGE - 1) != 0 {
                    return Err(EINVAL);
                }
                self.mappings.push(
                    self.uat.kernel_lower_vm().map_at(
                        lower,
                        bytes,
                        &object.gem(),
                        mmu::Prot::from_pte(pte::LOWER),
                        false,
                    )?,
                    GFP_KERNEL,
                )?;
                uat::clean_lower(&self.uat)?;
            }
            wmb();
            self.window
                .lock()
                .sync_range(&self.uat, page_va..page_va + bytes as u64)
        })();
        if let Err(error) = mapped {
            self.error = Some(error);
            return Err(error);
        }
        if req.client.is_some() {
            self.clients.push(object.clone(), GFP_KERNEL)?;
        }
        Ok(object)
    }

    /// Reserves a page-aligned loader aperture followed by an unmapped guard. This advances
    /// the same cursor as firmware objects, including holes in discontiguous loader entries.
    pub(crate) fn reserve_io(&mut self, size: u64) -> Result<u64> {
        if self.sealed {
            return Err(EBUSY);
        }
        if let Some(error) = self.error {
            return Err(error);
        }
        if size == 0 || size & (hw::PAGE - 1) != 0 {
            return Err(EINVAL);
        }
        let va = self.fw_next;
        let next = va
            .checked_add(size)
            .and_then(|end| end.checked_add(hw::PAGE))
            .ok_or(EOVERFLOW)?;
        if next > hw::va::FW_ALLOC.end {
            return Err(ENOSPC);
        }
        self.fw_next = next;
        Ok(va)
    }

    /// Maps a device range with the ordinary firmware device RW class.
    #[allow(dead_code)]
    pub(crate) fn map_mmio(&mut self, va: u64, pa: u64, size: u64) -> Result {
        self.map_mmio_class(va, pa, size, Class::Mmio.pte())
    }

    /// Maps one admitted J713 loader span with its exact access class. In particular the
    /// protected/RO loader entries and the FW-shared host-generation page must not be
    /// silently widened to device RW.
    pub(crate) fn map_mmio_class(&mut self, va: u64, pa: u64, size: u64, attrs: u64) -> Result {
        if self.sealed {
            return Err(EBUSY);
        }
        if let Some(error) = self.error {
            return Err(error);
        }
        let end = va.checked_add(size).ok_or(EOVERFLOW)?;
        let pa_end = pa.checked_add(size).ok_or(EOVERFLOW)?;
        let allowed = matches!(
            attrs,
            0x00c0000000000447 | 0x0080000000000447 | 0x00200000000004c7 | 0x00c000000000044b
        );
        let prot = mmu::Prot::from_pte(attrs);
        if !allowed
            || prot.leaf_bits() | 3 != attrs
            || size == 0
            || (va | pa | size) & (hw::PAGE - 1) != 0
            || va < hw::HOST
            || end > hw::HOST + hw::HOST_SIZE
            || pa_end - 1 > (hw::PA_MASK | (hw::PAGE - 1))
        {
            return Err(EINVAL);
        }
        self.mappings.reserve(1, GFP_KERNEL)?;
        let mapping = self.uat.kernel_vm().map_io(
            self.dev.as_ref().as_ref(),
            va,
            usize::try_from(pa)?,
            usize::try_from(size)?,
            prot,
        )?;
        self.mappings.push(mapping, GFP_KERNEL)?;
        let result = self.window.lock().sync_range(&self.uat, va..end);
        if let Err(error) = result {
            self.error = Some(error);
        }
        result
    }

    /// Boot-time check that the shared page-table code writes every firmware leaf word exactly:
    /// each class word is mapped on a scratch page with `Prot::from_pte(word)`, the leaf is read
    /// back from the tables and must equal `pa | word`. Host-window classes are checked in the
    /// (global) host window, the lower and client classes in the (non-global) context-0 lower
    /// tree, which user VMs share the leaf format with. Runs before anything firmware-visible is
    /// written; the scratch mappings are retained (they are never published). A mismatch refuses
    /// the boot.
    pub(crate) fn self_check(&mut self) -> Result {
        let window = [
            pte::FW,
            pte::SHARED,
            pte::FW_CACHED,
            pte::SHARED_CACHED,
            pte::MMIO,
        ];
        let lower = [
            pte::LOWER,
            pte::LEAF_NC,
            pte::LEAF_CACHED,
            pte::LEAF_PB_LIST,
        ];
        self.mappings
            .reserve(window.len() + lower.len(), GFP_KERNEL)?;
        let scratch = gem::new_kernel_object_wc(&self.dev, hw::PAGE as usize)?;
        // SAFETY: self-checks run before the bound platform probe returns.
        let bound = unsafe { self.dev.as_ref().as_ref().as_bound() };
        let sgt = gem::held_sg_table(&scratch.gem, bound)?;
        let pa = sgt.iter().next().ok_or(EINVAL)?.dma_address() as u64;
        let checks = Iterator::chain(
            window
                .iter()
                .enumerate()
                .map(|(i, &w)| (true, hw::va::SELF_CHECK + i as u64 * hw::PAGE, w)),
            lower
                .iter()
                .enumerate()
                .map(|(i, &w)| (false, hw::va::SELF_CHECK_LOWER + i as u64 * hw::PAGE, w)),
        );
        let mut count = 0;
        for (host, va, word) in checks {
            let (vm, root) = if host {
                (self.uat.kernel_vm(), self.uat.kernel_window_root())
            } else {
                (self.uat.kernel_lower_vm(), self.uat.kernel_lower_root())
            };
            let mapping = vm.map_at(
                va,
                hw::PAGE as usize,
                &scratch.gem,
                mmu::Prot::from_pte(word),
                false,
            )?;
            self.mappings.push(mapping, GFP_KERNEL)?;
            let leaf = uat::read_leaf(root, va)?;
            if leaf != Some(pa | word) {
                dev_err!(
                    self.dev.as_ref(),
                    "G16: UAT leaf self-check failed at {:#x}: want {:#x}, tables hold {:#x}\n",
                    va,
                    pa | word,
                    leaf.unwrap_or(0)
                );
                return Err(super::boot::eproto());
            }
            count += 1;
        }
        // The scratch page stays mapped: retain its backing with the mappings.
        let _ = sgt;
        core::mem::forget(scratch);
        dev_info!(
            self.dev.as_ref(),
            "G16: UAT leaf self-check passed ({} classes)\n",
            count
        );
        Ok(())
    }
}
