// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Retained USC private-memory pools and their submission counters.
//!
//! A logical context owns one render pool, shared by its render pairs. A
//! physical compute queue owns a separate pool. All mutation and publication
//! runs under the device mutex. Pool aliases must outlive every firmware
//! reference and are released only after the owner's teardown is acknowledged.

use super::fw::freelist::*;
use super::fw::initdata::{FreeListArgs, FreeListClass, FreeListControl, FREE_LIST_BLOCK_SIZE};
use super::object::{Allocator, CpuMap, KernelObject};
use crate::mmu;
use core::sync::atomic::{fence, AtomicBool, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc};

/// Render pools occupy 129..255; compute uses its QID plus one, in 1..128.
/// First-free allocation keeps the shared firmware namespace deterministic.
pub(crate) struct RenderIds {
    used: [AtomicU64; 2],
}
impl RenderIds {
    pub(crate) const fn new() -> Self {
        Self {
            used: [AtomicU64::new(0), AtomicU64::new(1 << 63)],
        }
    }
}
struct RenderId {
    slot: u16,
    allocator: Arc<RenderIds>,
}
impl RenderId {
    fn new(allocator: &Arc<RenderIds>) -> Result<Self> {
        for (bank, used) in allocator.used.iter().enumerate() {
            let mut value = used.load(Ordering::Acquire);
            loop {
                let bit = value.trailing_ones();
                if bit == 64 {
                    break;
                }
                match used.compare_exchange_weak(
                    value,
                    value | (1u64 << bit),
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => {
                        return Ok(Self {
                            slot: 129 + bank as u16 * 64 + bit as u16,
                            allocator: allocator.clone(),
                        })
                    }
                    Err(current) => value = current,
                }
            }
        }
        Err(ENOSPC)
    }
}
impl Drop for RenderId {
    fn drop(&mut self) {
        let bit = self.slot as usize - 129;
        self.allocator.used[bit / 64].fetch_and(!(1u64 << (bit % 64)), Ordering::Release);
    }
}

pub(crate) struct Pages {
    backing: KernelObject,
    blocks: usize,
    layout: RunLayout,
}
impl Pages {
    fn new(alloc: &Allocator<'_>, blocks: usize, layout: RunLayout) -> Result<Self> {
        let mut backing = alloc.lower(
            TABLE_SIZE + blocks * FREE_LIST_BLOCK_SIZE,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        let base = backing.gpu_va();
        backing.initialize::<PoolTables>(0, |tables| tables.init(base, blocks, layout))?;
        Ok(Self {
            backing,
            blocks,
            layout,
        })
    }
    /// Fresh zeroed compute backing, built without the device mutex.
    pub(crate) fn compute(alloc: &Allocator<'_>) -> Result<Self> {
        Self::new(alloc, COMPUTE_BLOCKS, RunLayout::Compute)
    }
    fn page_list_va(&self) -> u64 {
        self.backing.gpu_va()
    }
    fn run_list_va(&self) -> u64 {
        self.page_list_va() + PAGE_LIST_SIZE as u64
    }
    fn repopulate(&self) -> Result {
        let table = self.backing.pointer(0, TABLE_SIZE)?.cast::<u64>();
        PoolTables::for_each_word(
            self.page_list_va(),
            self.blocks,
            self.layout,
            |index, value| {
                // SAFETY: The page-aligned backing was validated for the complete
                // tables. The enumerator bounds every index inside those tables;
                // the owner has proven firmware release before this update.
                unsafe { table.add(index).write_volatile(value) };
            },
        )
    }
}

/// Pool-object identities are independent of recycled firmware IDs and VAs.
static RENDER_POOL_SERIAL: AtomicU64 = AtomicU64::new(0);

/// A render pool retained by a logical context and every graph referencing it.
pub(crate) struct RenderPool {
    // Remove all aliases before dropping the backing or returning the ID.
    _client: mmu::KernelMapping,
    _global: mmu::KernelMapping,
    pages: Pages,
    control: KernelObject,
    state: KernelObject,
    served: AtomicBool,
    serial: u64,
    id: RenderId,
}

impl RenderPool {
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        ids: &Arc<RenderIds>,
        render_global: &mmu::Vm,
        client: &mmu::Vm,
    ) -> Result<Self> {
        let id = RenderId::new(ids)?;
        let pages = Pages::new(alloc, RENDER_BLOCKS, RunLayout::Render)?;
        let address = pages.page_list_va();
        let global = pages
            .backing
            .map_alias(render_global, address, mmu::PROT_GPU_SHARED_RW)?;
        let client = pages
            .backing
            .map_alias(client, address, mmu::PROT_GPU_SHARED_RW)?;
        let state = alloc.kernel(
            CONTROL_SIZE,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        let mut control = alloc.kernel(
            CONTROL_SIZE,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        control.write(
            0,
            FreeListControl::render(
                &FreeListArgs {
                    buffer_slot: u32::from(id.slot),
                    class: FreeListClass::Two,
                    page_list_va: address,
                    run_list_va: pages.run_list_va(),
                    blocks: RENDER_BLOCKS as u32,
                    state_va: state.gpu_va(),
                },
                0,
            ),
        )?;
        fence(Ordering::SeqCst);
        let serial = RENDER_POOL_SERIAL
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(1))
            .map_err(|_| EOVERFLOW)? + 1;
        Ok(Self {
            _client: client,
            _global: global,
            pages,
            control,
            state,
            served: AtomicBool::new(false),
            serial,
            id,
        })
    }

    pub(crate) fn serial(&self) -> u64 {
        self.serial
    }
    pub(crate) fn id(&self) -> u16 {
        self.id.slot
    }
    pub(crate) fn control_va(&self) -> u64 {
        self.control.gpu_va()
    }
    pub(crate) fn page_list_va(&self) -> u64 {
        self.pages.page_list_va()
    }
    pub(crate) fn run_list_va(&self) -> u64 {
        self.pages.run_list_va()
    }
    pub(crate) fn end_va(&self) -> u64 {
        self.run_list_va()
            + (RENDER_BLOCKS * size_of::<super::fw::initdata::FreeListRunSlot>()) as u64
    }

    /// Fresh pools are already populated. Reuse requires the previous release
    /// to have been consumed; command completion alone does not establish it.
    pub(crate) fn repopulate(&self) -> Result {
        if !self.served.swap(true, Ordering::AcqRel) {
            return Ok(());
        }
        let submitted = self.state.dword(0)?.load(Ordering::Relaxed);
        let header = FreeListControl::render(
            &FreeListArgs {
                buffer_slot: u32::from(self.id()),
                class: FreeListClass::Two,
                page_list_va: self.page_list_va(),
                run_list_va: self.run_list_va(),
                blocks: RENDER_BLOCKS as u32,
                state_va: self.state.gpu_va(),
            },
            submitted,
        );
        let mut image = KVVec::from_elem(0u64, CONTROL_SIZE / 8, GFP_KERNEL)?;
        // SAFETY: `image` owns a zeroed control page, is large enough for the
        // header, and every header byte is initialized integer data.
        unsafe {
            image
                .as_mut_ptr()
                .cast::<FreeListControl>()
                .write_unaligned(header)
        };
        let control = self.control.pointer(0, CONTROL_SIZE)?.cast::<u64>();
        self.pages.repopulate()?;
        for (index, value) in image.iter().enumerate() {
            // SAFETY: The entire retained page was checked before any store;
            // the allocation is page-aligned and release excludes the firmware.
            unsafe { control.add(index).write_volatile(*value) };
        }
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Submitted is host-owned and aligned. Completed is the packed firmware
    /// counter, so retain the byte accesses required by that interface.
    pub(crate) fn counters(&self) -> Result<[u64; 2]> {
        let submitted = self.state.dword(0)?.load(Ordering::Relaxed);
        let completed = self.control.pointer(FreeListControl::COMPLETED, 8)?;
        let mut bytes = [0; 8];
        for (index, byte) in bytes.iter_mut().enumerate() {
            // SAFETY: The checked eight-byte range stays mapped; byte accesses
            // permit the unaligned counter without widening shared accesses.
            *byte = unsafe { completed.add(index).read_volatile() };
        }
        Ok([submitted, u64::from_le_bytes(bytes)])
    }

    /// Snapshot the unaligned shared fields before loading their descriptor.
    pub(crate) fn page_pool_state(&self) -> Result<PagePoolState> {
        let raw = self.control.pointer(0x14, 0x1c)?;
        fence(Ordering::Acquire);
        let read = |offset: usize, count: usize| -> u64 {
            let mut bytes = [0; 8];
            for (index, byte) in bytes.iter_mut().take(count).enumerate() {
                // SAFETY: The complete 0x14..0x30 span was checked. These fixed
                // field ranges lie inside it; byte loads permit unaligned words.
                *byte = unsafe { raw.add(offset + index).read_volatile() };
            }
            u64::from_le_bytes(bytes)
        };
        Ok(PagePoolState {
            page_list_va: read(0, 8),
            unk_1c: read(8, 4) as u32,
            unk_20: read(12, 4) as u32,
            unk_24: read(16, 4) as u32,
            unk_28: read(20, 4) as u32,
            page_count: read(24, 4) as u32,
        })
    }

    /// Save mutable descriptor state only after both engines and free-list
    /// references retire, before the physical slot changes owners.
    pub(crate) fn store_page_pool_state(&self, state: PagePoolState) -> Result {
        let raw = self.control.pointer(0x20, 0x10)?;
        for (word, value) in [state.unk_20, state.unk_24, state.unk_28, state.page_count]
            .into_iter()
            .enumerate()
        {
            for (index, byte) in value.to_le_bytes().into_iter().enumerate() {
                // SAFETY: The entire mutable range was checked, and each
                // enumerated byte lies inside it. Caller owns the retired pool.
                unsafe { raw.add(word * 4 + index).write_volatile(byte) };
            }
        }
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Prepares the counter before any producer is written. Commit belongs to
    /// the publication transaction; dropping this plan does not reserve work.
    pub(crate) fn prepare_submission(&self, commands: u32) -> Result<Submitted<'_>> {
        if commands > 2 {
            return Err(EINVAL);
        }
        let counter = self.state.dword(0)?;
        Ok(Submitted {
            counter,
            next: counter
                .load(Ordering::Relaxed)
                .wrapping_add(u64::from(commands)),
        })
    }
}

/// One checked, infallible counter update in a submission transaction.
pub(crate) struct Submitted<'a> {
    counter: &'a AtomicU64,
    next: u64,
}
impl Submitted<'_> {
    pub(crate) fn commit(self) {
        self.counter.store(self.next, Ordering::Relaxed);
        fence(Ordering::SeqCst);
    }
}

/// A pool owned by one physical compute queue, with fixed QID-derived slot.
/// Idle retained queues may give their USC backing back (`pages` is `None`)
/// once the firmware consumed the pool's release; the control and state
/// objects the queue record names stay until processor stop.
pub(crate) struct ComputePool {
    pages: Option<Pages>,
    control: KernelObject,
    state: KernelObject,
    id: u16,
}
impl ComputePool {
    pub(crate) fn new(alloc: &Allocator<'_>, qid: u8) -> Result<Self> {
        if qid > super::fw::kick::QID_MAX {
            return Err(EINVAL);
        }
        let id = u16::from(qid) + 1;
        let pages = Pages::new(alloc, COMPUTE_BLOCKS, RunLayout::Compute)?;
        let mut control = alloc.kernel(
            CONTROL_SIZE,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        let state = alloc.kernel(
            CONTROL_SIZE,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        control.write(
            0,
            FreeListControl::compute(&FreeListArgs {
                buffer_slot: u32::from(id),
                class: FreeListClass::One,
                page_list_va: pages.page_list_va(),
                run_list_va: pages.run_list_va(),
                blocks: COMPUTE_BLOCKS as u32,
                state_va: state.gpu_va(),
            }),
        )?;
        fence(Ordering::SeqCst);
        Ok(Self {
            pages: Some(pages),
            control,
            state,
            id,
        })
    }
    pub(crate) fn id(&self) -> u16 {
        self.id
    }
    /// EAGAIN while the pool is vacant: the owner must re-arm it first.
    pub(crate) fn page_list_va(&self) -> Result<u64> {
        self.pages.as_ref().map(Pages::page_list_va).ok_or(EAGAIN)
    }
    pub(crate) fn control_va(&self) -> u64 {
        self.control.gpu_va()
    }
    pub(crate) fn populated(&self) -> bool {
        self.pages.is_some()
    }
    /// The release record for this pool's buffer slot under the free-list
    /// generation the queue last published.
    pub(crate) fn release_record(&self, generation: u64) -> super::fw::channels::FreeListRelease {
        super::fw::channels::FreeListRelease::new(generation, u32::from(self.id))
    }
    /// The caller has observed the firmware consume this pool's release.
    pub(crate) fn drop_pages(&mut self) {
        self.pages = None;
    }
    /// Rewrites the shared control header for fresh backing. The caller then
    /// rewrites the page-pool descriptor row before the queue is kicked again.
    /// As render repopulation does, the firmware-completed counter is carried
    /// over so it stays consistent with the host submitted counter in `state`.
    pub(crate) fn rearm(&mut self, pages: Pages) -> Result {
        if self.pages.is_some() {
            return Err(EBUSY);
        }
        let completed = self.control.pointer(FreeListControl::COMPLETED, 8)?;
        let mut bytes = [0; 8];
        for (index, byte) in bytes.iter_mut().enumerate() {
            // SAFETY: The checked eight-byte range stays mapped; byte accesses
            // match the unaligned packed counter the firmware writes.
            *byte = unsafe { completed.add(index).read_volatile() };
        }
        let mut header = FreeListControl::compute(&FreeListArgs {
            buffer_slot: u32::from(self.id),
            class: FreeListClass::One,
            page_list_va: pages.page_list_va(),
            run_list_va: pages.run_list_va(),
            blocks: COMPUTE_BLOCKS as u32,
            state_va: self.state.gpu_va(),
        });
        header.unk_54[..8].copy_from_slice(&bytes);
        self.control.write(0, header)?;
        fence(Ordering::SeqCst);
        self.pages = Some(pages);
        Ok(())
    }
    pub(crate) fn map_client(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        let pages = self.pages.as_mut().ok_or(EAGAIN)?;
        let address = pages.page_list_va();
        pages
            .backing
            .map_alias(vm, address, mmu::PROT_GPU_SHARED_RW)
    }
    pub(crate) fn reserve_submission(&self) -> Result<u64> {
        let counter = self.state.dword(0)?;
        let next = counter
            .load(Ordering::Relaxed)
            .checked_add(1)
            .ok_or(EOVERFLOW)?;
        counter.store(next, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        Ok(next)
    }
    /// The caller has proved no pointer from this submission was published.
    pub(crate) fn cancel_unpublished(&self, target: u64) -> Result {
        let counter = self.state.dword(0)?;
        if target == 0 || counter.load(Ordering::Acquire) != target {
            return Err(EINVAL);
        }
        counter.store(target - 1, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        Ok(())
    }
}

/// Registration identity held by a free-list lease. The cookie is the end of
/// the run list; it remains stable while overlapping command pairs share it.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Owner {
    /// Identifies the host pool object even when every firmware address recycles.
    pub(crate) serial: u64,
    pub(crate) buffer_slot: u32,
    pub(crate) control_va: u64,
    pub(crate) ring_va: u64,
    pub(crate) cookie: u64,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum LeasePhase {
    Preparing,
    Ready,
    ReleaseSent(u32),
    Released,
}

/// References to one prepopulated owner: preparation and two engine commands
/// per render. Published releases do not gate reacquisition; their consumption
/// witnesses are carried into the successor.
#[derive(Copy, Clone, Debug)]
pub(crate) struct Lease {
    owner: Owner,
    generation: u64,
    references: u32,
    commands: u32,
    phase: LeasePhase,
    previous_release: Option<u32>,
}

impl Lease {
    /// Acquires an unchanged overlapping owner, or an owner whose previous
    /// release was sent. A changed owner additionally requires caller-side
    /// proof of old-pool release before its backing can be repopulated.
    pub(crate) fn acquire(previous: Option<&Self>, owner: Owner) -> Result<Self> {
        let Some(previous) = previous else {
            return Ok(Self {
                owner,
                generation: 0,
                references: 3,
                commands: 2,
                phase: LeasePhase::Preparing,
                previous_release: None,
            });
        };
        if previous.owner.buffer_slot != owner.buffer_slot {
            return Err(EINVAL);
        }
        let generation = previous.generation.checked_add(1).ok_or(EOVERFLOW)?;
        let same_owner = previous.owner == owner;
        let overlapping = same_owner
            && previous.phase == LeasePhase::Ready
            && previous.references >= 2
            && previous.commands >= 2;
        let reacquire = previous.reacquirable();
        if !overlapping && !reacquire {
            return Err(EBUSY);
        }
        let references = previous.references.checked_add(3).ok_or(EOVERFLOW)?;
        let commands = previous.commands.checked_add(2).ok_or(EOVERFLOW)?;
        Ok(Self {
            owner,
            generation: if overlapping {
                previous.generation
            } else {
                generation
            },
            references,
            commands,
            phase: LeasePhase::Preparing,
            previous_release: previous.release_witness(),
        })
    }

    pub(crate) fn owner(&self) -> Owner {
        self.owner
    }
    pub(crate) fn generation(&self) -> u64 {
        self.generation
    }
    pub(crate) fn reacquirable(&self) -> bool {
        matches!(
            self.phase,
            LeasePhase::ReleaseSent(_) | LeasePhase::Released
        ) && self.references == 0
            && self.commands == 0
    }
    pub(crate) fn released(&self) -> bool {
        self.phase == LeasePhase::Released && self.previous_release.is_none()
    }
    pub(crate) fn release_witness(&self) -> Option<u32> {
        match self.phase {
            LeasePhase::ReleaseSent(cursor) => Some(cursor),
            _ => self.previous_release,
        }
    }

    /// Publication retires preparation independently of the command references.
    pub(crate) fn retire_prepare(&mut self) -> Result {
        if self.phase != LeasePhase::Preparing || self.references < 3 {
            return Err(EINVAL);
        }
        self.references -= 1;
        self.phase = LeasePhase::Ready;
        Ok(())
    }
    /// Fixed prepopulated pools never submit an asynchronous growth request.
    /// An incoming growth completion therefore cannot name a live request.
    pub(crate) fn complete_grow(
        &mut self,
        _event: super::fw::channels::FreeListCompletion,
    ) -> Result {
        Err(EIO)
    }
    /// Both engines have retired this render's exact retained submission.
    pub(crate) fn complete_render(
        &mut self,
    ) -> Result<Option<super::fw::channels::FreeListRelease>> {
        if self.phase != LeasePhase::Ready || self.references < 2 || self.commands < 2 {
            return Err(EINVAL);
        }
        self.references -= 2;
        self.commands -= 2;
        Ok(self.pending_release())
    }
    pub(crate) fn pending_release(&self) -> Option<super::fw::channels::FreeListRelease> {
        (self.phase == LeasePhase::Ready && self.references == 0).then(|| {
            super::fw::channels::FreeListRelease::new(self.generation, self.owner.buffer_slot)
        })
    }
    /// Records the producer-after cursor once the final release was published.
    pub(crate) fn mark_release_sent(&mut self, cursor: u32) -> Result {
        if self.pending_release().is_none() || cursor >= super::fw::channels::DEVICE_CONTROL_SLOTS {
            return Err(EINVAL);
        }
        self.phase = LeasePhase::ReleaseSent(cursor);
        Ok(())
    }
    /// Observes all older release records passed by one consumer snapshot.
    pub(crate) fn observe_releases(&mut self, consumer: u32, producer: u32) -> Result {
        if let Some(cursor) = self.previous_release {
            if control_consumed(consumer, producer, cursor)? {
                self.previous_release = None;
            }
        }
        if let LeasePhase::ReleaseSent(cursor) = self.phase {
            if control_consumed(consumer, producer, cursor)? {
                self.phase = LeasePhase::Released;
            }
        }
        Ok(())
    }
}

/// A publication is retired once its producer-after cursor leaves the
/// current `(consumer, producer]` interval, including wrapped intervals.
pub(crate) fn control_consumed(consumer: u32, producer: u32, cursor: u32) -> Result<bool> {
    let slots = super::fw::channels::DEVICE_CONTROL_SLOTS;
    if consumer >= slots || producer >= slots || cursor >= slots {
        return Err(EIO);
    }
    let pending = producer.wrapping_sub(consumer) & (slots - 1);
    let ahead = cursor.wrapping_sub(consumer) & (slots - 1);
    Ok(ahead == 0 || ahead > pending)
}
