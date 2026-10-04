// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! GPU ring buffer channels
//!
//! The GPU firmware use a set of ring buffer channels to receive commands from the driver and send
//! it notifications and status messages.
//!
//! These ring buffers mostly follow uniform conventions, so they share the same base
//! implementation.

use crate::debug::*;
use crate::driver::{
    AsahiDevRef,
    AsahiDevice, //
};
use crate::fw::channels::*;
use crate::fw::initdata::{
    raw,
    ChannelRing, //
};
use crate::fw::types::*;
use crate::{
    buffer,
    event,
    gpu,
    mem, //
};
use core::ptr::{
    self,
    NonNull, //
};
use kernel::{
    bindings, c_str,
    prelude::*,
    sync::Arc,
    time::{
        delay::fsleep,
        Delta,
        Instant,
        Monotonic, //
    },
};

pub(crate) use crate::fw::channels::PipeType;

/// Number of firmware RX messages (events, FWLog, RTKit doorbells) logged at info level during
/// G15 bring-up before falling back to the normal debug-flag controlled logging.
const G15_RX_LOG_BUDGET: u32 = 256;
static G15_RX_LOGGED: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/// Returns true while the G15 info-level RX log budget lasts (bounded bring-up logging), and
/// logs once when it runs out.
pub(crate) fn g15_rx_log_budget() -> bool {
    let n = G15_RX_LOGGED.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
    if n == G15_RX_LOG_BUDGET {
        pr_info!(
            "asahi: G15: RX message log budget ({}) used up; further messages only with debug flags\n",
            G15_RX_LOG_BUDGET
        );
    }
    n < G15_RX_LOG_BUDGET
}

/// Snapshot of the three index words of a 14.x driver->FW ring.
#[derive(Debug, Copy, Clone)]
#[allow(dead_code)] // Fields are only read through Debug.
pub(crate) struct RingIndices {
    /// FW-written read index.
    pub(crate) rptr: u32,
    /// FW-written CFI index.
    pub(crate) cfi: u32,
    /// Host-written write index as it reads back from memory.
    pub(crate) wptr: u32,
    /// The host's own copy of the write index.
    pub(crate) wptr_host: u32,
}

/// A receive (FW->driver) channel.
pub(crate) struct RxChannel<T: RxChannelState, U: Copy + Default>
where
    for<'a> <T as GpuStruct>::Raw<'a>: Debug + Default + Zeroable,
{
    ring: ChannelRing<T, U>,
    // FIXME: needs feature(generic_const_exprs)
    //rptr: [u32; T::SUB_CHANNELS],
    rptr: [u32; 6],
    count: u32,
}

impl<T: RxChannelState, U: Copy + Default> RxChannel<T, U>
where
    for<'a> <T as GpuStruct>::Raw<'a>: Debug + Default + Zeroable,
{
    /// Allocates a new receive channel with a given message count.
    pub(crate) fn new(alloc: &mut gpu::KernelAllocators, count: usize) -> Result<RxChannel<T, U>> {
        Ok(RxChannel {
            ring: ChannelRing {
                state: alloc.shared.new_default()?,
                ring: alloc.shared.array_empty(T::SUB_CHANNELS * count)?,
            },
            rptr: Default::default(),
            count: count as u32,
        })
    }

    /// Receives a message on the specified sub-channel index, optionally leaving in the ring
    /// buffer.
    ///
    /// Returns None if the channel is empty.
    fn get_or_peek(&mut self, index: usize, peek: bool) -> Option<U> {
        self.ring.state.with(|raw, _inner| {
            let wptr = T::wptr(raw, index);
            let rptr = &mut self.rptr[index];
            if wptr == *rptr {
                None
            } else {
                let off = self.count as usize * index;
                let msg = self.ring.ring[off + *rptr as usize];
                if !peek {
                    *rptr = (*rptr + 1) % self.count;
                    T::set_rptr(raw, index, *rptr);
                }
                Some(msg)
            }
        })
    }

    /// Read-only snapshot of (firmware wptr, host rptr) per sub-channel (G15 bring-up diagnostics).
    pub(crate) fn snapshot(&self) -> KVec<(u32, u32)> {
        let mut out = KVec::new();
        self.ring.state.with(|raw, _inner| {
            for i in 0..T::SUB_CHANNELS.min(self.rptr.len()) {
                let _ = out.push((T::wptr(raw, i), self.rptr[i]), GFP_KERNEL);
            }
        });
        out
    }

    /// Receives a message on the specified sub-channel index, and dequeues it from the ring buffer.
    ///
    /// Returns None if the channel is empty.
    pub(crate) fn get(&mut self, index: usize) -> Option<U> {
        self.get_or_peek(index, false)
    }

    /// Peeks a message on the specified sub-channel index, leaving it in the ring buffer.
    ///
    /// Returns None if the channel is empty.
    pub(crate) fn peek(&mut self, index: usize) -> Option<U> {
        self.get_or_peek(index, true)
    }
}

/// A transmit (driver->FW) channel.
pub(crate) struct TxChannel<T: TxChannelState, U: Copy + Default>
where
    for<'a> <T as GpuStruct>::Raw<'a>: Debug + Default + Zeroable,
{
    ring: ChannelRing<T, U>,
    wptr: u32,
    count: u32,
}

impl<T: TxChannelState, U: Copy + Default> TxChannel<T, U>
where
    for<'a> <T as GpuStruct>::Raw<'a>: Debug + Default + Zeroable,
{
    /// Allocates a new cached transmit channel with a given message count.
    pub(crate) fn new(alloc: &mut gpu::KernelAllocators, count: usize) -> Result<TxChannel<T, U>> {
        Ok(TxChannel {
            ring: ChannelRing {
                state: alloc.shared.new_default()?,
                ring: alloc.private.array_empty(count)?,
            },
            wptr: 0,
            count: count as u32,
        })
    }

    /// Allocates a new uncached transmit channel with a given message count.
    pub(crate) fn new_uncached(
        alloc: &mut gpu::KernelAllocators,
        count: usize,
    ) -> Result<TxChannel<T, U>> {
        Ok(TxChannel {
            ring: ChannelRing {
                state: alloc.shared.new_default()?,
                ring: alloc.shared.array_empty(count)?,
            },
            wptr: 0,
            count: count as u32,
        })
    }

    /// Send a message to the ring, returning a cookie with the ring buffer position.
    ///
    /// This will poll/block if the ring is full, which we don't really expect to happen.
    pub(crate) fn put(&mut self, msg: &U) -> u32 {
        self.ring.state.with(|raw, _inner| {
            let next_wptr = (self.wptr + 1) % self.count;
            let mut rptr = T::rptr(raw);
            if next_wptr == rptr {
                pr_err!(
                    "TX ring buffer is full! Waiting... ({}, {})\n",
                    next_wptr,
                    rptr
                );
                // TODO: block properly on incoming messages?
                while next_wptr == rptr {
                    fsleep(Delta::from_millis(8));
                    rptr = T::rptr(raw);
                }
            }
            self.ring.ring[self.wptr as usize] = *msg;
            mem::sync();
            T::set_wptr(raw, next_wptr);
            self.wptr = next_wptr;
        });
        self.wptr
    }

    /// Wait for a previously submitted message to be popped off of the ring by the GPU firmware.
    ///
    /// This busy-loops, and is intended to be used for rare cases when we need to block for
    /// completion of a cache management or invalidation operation synchronously (which
    /// the firmware normally completes fast enough not to be worth sleeping for).
    /// If the poll takes longer than 10ms, this switches to sleeping between polls.
    pub(crate) fn wait_for(&mut self, wptr: u32, timeout_ms: i64) -> Result {
        const MAX_FAST_POLL: i64 = 10;
        let start = Instant::<Monotonic>::now();
        let timeout_ms = timeout_ms.max(1);
        let timeout_fast = Delta::from_millis(timeout_ms.min(MAX_FAST_POLL));
        let timeout_slow = Delta::from_millis(timeout_ms);
        self.ring.state.with(|raw, _inner| {
            while start.elapsed() < timeout_fast {
                if T::rptr(raw) == wptr {
                    return Ok(());
                }
                mem::sync();
            }
            while start.elapsed() < timeout_slow {
                if T::rptr(raw) == wptr {
                    return Ok(());
                }
                fsleep(Delta::from_millis(5));
                mem::sync();
            }
            Err(ETIMEDOUT)
        })
    }
}

/// Fender scratch SRAM layout: part A (host-written: write indices and pipe rings) is
/// roundup(0x10000, page) + one page, part B (FW-written: read and CFI indices) is the rest of
/// the 128 KiB region at Fender+0x60000.
const SCRATCH_PART_A_SIZE: usize = 0x14000;
/// See [`SCRATCH_PART_A_SIZE`].
const SCRATCH_PART_B_SIZE: usize = 0xc000;

/// CPU mapping of the Fender scratch SRAM.
struct ScratchRamMap {
    cpu: NonNull<u8>,
}

// SAFETY: This is a plain MMIO mapping. All accesses go through volatile reads/writes of
// naturally aligned words, and the mapping itself is immutable.
unsafe impl Send for ScratchRamMap {}
// SAFETY: See above.
unsafe impl Sync for ScratchRamMap {}

impl Drop for ScratchRamMap {
    fn drop(&mut self) {
        // SAFETY: `cpu` was returned by a successful `ioremap_np()` in `ScratchRam::new()`, and
        // every channel using the mapping holds an `Arc` to it.
        unsafe { bindings::iounmap(self.cpu.as_ptr() as *mut core::ffi::c_void) };
    }
}

/// Which half of the scratch SRAM an allocation goes into.
#[derive(Copy, Clone)]
enum ScratchPart {
    /// Host-written words: write indices and pipe rings.
    A,
    /// FW-written words: read and CFI indices.
    B,
}

/// Allocator for the G15 Fender scratch SRAM, where the 14.x firmware on G15 expects the pipe
/// rings and the index words of every host->FW ring.
///
/// Allocations are 8-byte aligned and ring storage is never freed, so this is a bump
/// allocator.
///
/// TODO: the SRAM contents do not survive a GPU power-down; they must be copied to a DRAM
/// shadow before power-down and back on power-up. The G15 path keeps the GPU powered instead,
/// so there is no backup/restore here yet.
/// TODO: part A and part B may need different GPU cache attributes; the caller currently maps
/// the whole region once.
pub(crate) struct ScratchRam {
    map: Arc<ScratchRamMap>,
    gpu_base: u64,
    next_a: usize,
    next_b: usize,
}

#[allow(dead_code)] // Only the G15 (14.x) instantiation uses the SRAM placement.
impl ScratchRam {
    /// Map the scratch SRAM for the CPU and prepare to allocate from it.
    ///
    /// # Safety
    ///
    /// `phys`/`size` must describe the Fender scratch SRAM (`HwConfig::sram_base/sram_size`) and
    /// `gpu_base` must be a firmware VA mapping that whole region, which the caller keeps mapped
    /// for as long as this `ScratchRam` or any channel placed in it exists.
    pub(crate) unsafe fn new(phys: usize, size: usize, gpu_base: u64) -> Result<ScratchRam> {
        if size < SCRATCH_PART_A_SIZE + SCRATCH_PART_B_SIZE {
            return Err(EINVAL);
        }
        // SAFETY: The caller guarantees `phys`/`size` is the SRAM region. Apple SoC MMIO needs
        // non-posted mappings.
        let cpu = unsafe { bindings::ioremap_np(phys as bindings::phys_addr_t, size) };
        let cpu = NonNull::new(cpu as *mut u8).ok_or(ENOMEM)?;
        Ok(ScratchRam {
            map: Arc::new(ScratchRamMap { cpu }, GFP_KERNEL)?,
            gpu_base,
            next_a: 0,
            next_b: SCRATCH_PART_A_SIZE,
        })
    }

    /// Allocate `size` bytes (8-byte aligned) and zero them. Returns the CPU pointer
    /// and the firmware VA.
    fn alloc(&mut self, part: ScratchPart, size: usize) -> Result<(*mut u8, u64)> {
        let (next, end) = match part {
            ScratchPart::A => (&mut self.next_a, SCRATCH_PART_A_SIZE),
            ScratchPart::B => (&mut self.next_b, SCRATCH_PART_A_SIZE + SCRATCH_PART_B_SIZE),
        };
        let off = (*next + 7) & !7;
        let new_next = off.checked_add((size + 7) & !7).ok_or(EINVAL)?;
        if new_next > end {
            return Err(ENOSPC);
        }
        *next = new_next;

        // SAFETY: `off..new_next` is inside the mapping (checked above).
        let cpu = unsafe { self.map.cpu.as_ptr().add(off) };
        for i in 0..(new_next - off) / 8 {
            // SAFETY: 8-byte aligned words inside the allocation.
            unsafe { ptr::write_volatile((cpu as *mut u64).add(i), 0) };
        }
        Ok((cpu, self.gpu_base + off as u64))
    }
}

/// A driver->FW ring of the 14.x firmware ABI.
///
/// Each ring has three u32 index words (read and CFI written by the firmware, write written by
/// the host) and is described to the firmware by their FW VAs plus the ring VA
/// ([`raw::ScratchRingDesc`]). Index semantics are the G13 ones: 256 entries, full when
/// `(wptr + 1) % count == rptr`.
///
/// A new channel keeps its words in DRAM, the placement the 14.x G14X firmware uses. On G15,
/// [`ScratchTxChannel::place_in_sram`] moves them into the Fender scratch SRAM before the
/// firmware boots.
pub(crate) struct ScratchTxChannel<U: Copy + Default> {
    rptr_cpu: *mut u32,
    cfi_cpu: *mut u32,
    wptr_cpu: *mut u32,
    ring_cpu: *mut U,
    desc: raw::ScratchRingDesc,
    // Keep-alive for whichever memory the words and ring currently live in.
    dram_idx: Option<GpuArray<u32>>,
    dram_ring: Option<GpuArray<U>>,
    sram: Option<Arc<ScratchRamMap>>,
    wptr: u32,
    count: u32,
}

// SAFETY: The raw pointers point into memory owned by (or kept alive by) this object, and all
// accesses happen through `&mut self` or volatile reads of FW-written words.
unsafe impl<U: Copy + Default + Send> Send for ScratchTxChannel<U> {}
// SAFETY: See above.
unsafe impl<U: Copy + Default + Sync> Sync for ScratchTxChannel<U> {}

impl<U: Copy + Default> ScratchTxChannel<U> {
    /// DRAM word offsets of the three indices, one per 32-byte line like `ChannelState`.
    const DRAM_RPTR: usize = 0;
    const DRAM_CFI: usize = 8;
    const DRAM_WPTR: usize = 16;

    /// Allocate a new channel with `count` entries, with its indices and ring in DRAM.
    pub(crate) fn new(alloc: &mut gpu::KernelAllocators, count: usize) -> Result<Self> {
        // Entries are copied as u64 words (the SRAM is accessed 8 bytes at a time).
        if core::mem::size_of::<U>() % 8 != 0 {
            return Err(EINVAL);
        }
        let mut idx: GpuArray<u32> = alloc.shared.array_empty(24)?;
        let mut ring: GpuArray<U> = alloc.private.array_empty(count)?;
        let idx_va = idx.gpu_va().get();
        let idx_cpu = idx.as_mut_slice().as_mut_ptr();
        let ring_cpu = ring.as_mut_slice().as_mut_ptr();
        if ring_cpu as usize % 8 != 0 {
            return Err(EINVAL);
        }

        Ok(ScratchTxChannel {
            // SAFETY: All three offsets are inside the 24-word array.
            rptr_cpu: unsafe { idx_cpu.add(Self::DRAM_RPTR) },
            // SAFETY: See above.
            cfi_cpu: unsafe { idx_cpu.add(Self::DRAM_CFI) },
            // SAFETY: See above.
            wptr_cpu: unsafe { idx_cpu.add(Self::DRAM_WPTR) },
            ring_cpu,
            desc: raw::ScratchRingDesc {
                read_index: U64(idx_va + (Self::DRAM_RPTR * 4) as u64),
                cfi_index: U64(idx_va + (Self::DRAM_CFI * 4) as u64),
                write_index: U64(idx_va + (Self::DRAM_WPTR * 4) as u64),
                ring: U64(ring.gpu_va().get()),
            },
            dram_idx: Some(idx),
            dram_ring: Some(ring),
            sram: None,
            wptr: 0,
            count: count as u32,
        })
    }

    /// Move the index words (and, for the pipe rings, the ring itself) into the Fender scratch
    /// SRAM: write index in part A, read and CFI indices in part B, pipe ring entries in part A;
    /// the DeviceControl ring stays in DRAM.
    ///
    /// Must be called before the firmware boots and before `to_raw()` is written to
    /// RuntimePointers, since the descriptor changes.
    #[allow(dead_code)] // Only the G15 (14.x) instantiation uses the SRAM placement.
    pub(crate) fn place_in_sram(&mut self, sram: &mut ScratchRam, ring_in_sram: bool) -> Result {
        let (wptr_cpu, wptr_va) = sram.alloc(ScratchPart::A, 4)?;
        let (rptr_cpu, rptr_va) = sram.alloc(ScratchPart::B, 4)?;
        let (cfi_cpu, cfi_va) = sram.alloc(ScratchPart::B, 4)?;

        if ring_in_sram {
            let size = core::mem::size_of::<U>() * self.count as usize;
            let (ring_cpu, ring_va) = sram.alloc(ScratchPart::A, size)?;
            // Carry over anything already queued (normally nothing: this runs before boot).
            for i in 0..size / 8 {
                // SAFETY: Both rings are `size` bytes and 8-byte aligned.
                unsafe {
                    let v = ptr::read_volatile((self.ring_cpu as *const u64).add(i));
                    ptr::write_volatile((ring_cpu as *mut u64).add(i), v);
                }
            }
            self.ring_cpu = ring_cpu as *mut U;
            self.desc.ring = U64(ring_va);
            self.dram_ring = None;
        }

        self.rptr_cpu = rptr_cpu as *mut u32;
        self.cfi_cpu = cfi_cpu as *mut u32;
        self.wptr_cpu = wptr_cpu as *mut u32;
        // SAFETY: Valid SRAM words allocated above.
        unsafe {
            ptr::write_volatile(self.rptr_cpu, self.wptr);
            ptr::write_volatile(self.wptr_cpu, self.wptr);
        }
        self.desc.read_index = U64(rptr_va);
        self.desc.cfi_index = U64(cfi_va);
        self.desc.write_index = U64(wptr_va);
        self.dram_idx = None;
        self.sram = Some(sram.map.clone());
        Ok(())
    }

    /// Returns the ring descriptor to pass to the firmware.
    pub(crate) fn to_raw(&self) -> raw::ScratchRingDesc {
        self.desc
    }

    /// Returns the current write pointer: a `wait_for()` cookie for "everything queued so far",
    /// for callers that decide not to send a message.
    pub(crate) fn put_cookie(&self) -> u32 {
        self.wptr
    }

    fn rptr(&self) -> u32 {
        // SAFETY: `rptr_cpu` points to a live, aligned u32 (DRAM or SRAM).
        unsafe { ptr::read_volatile(self.rptr_cpu) }
    }

    /// Read-only snapshot of the ring index words (DRAM or Fender SRAM).
    pub(crate) fn indices(&self) -> RingIndices {
        // SAFETY: All three pointers point to live, aligned u32 words (DRAM or SRAM).
        unsafe {
            RingIndices {
                rptr: ptr::read_volatile(self.rptr_cpu),
                cfi: ptr::read_volatile(self.cfi_cpu),
                wptr: ptr::read_volatile(self.wptr_cpu),
                wptr_host: self.wptr,
            }
        }
    }

    /// Send a message to the ring, returning a cookie with the ring buffer position.
    ///
    /// This will poll/block if the ring is full, which we don't really expect to happen.
    pub(crate) fn put(&mut self, msg: &U) -> u32 {
        let next_wptr = (self.wptr + 1) % self.count;
        let mut rptr = self.rptr();
        if next_wptr == rptr {
            pr_err!(
                "TX ring buffer is full! Waiting... ({}, {})\n",
                next_wptr,
                rptr
            );
            // TODO: block properly on incoming messages?
            while next_wptr == rptr {
                fsleep(Delta::from_millis(8));
                rptr = self.rptr();
            }
        }

        let src = msg as *const U as *const u64;
        // SAFETY: `wptr < count`, so the entry is inside the ring, which is 8-byte aligned with an
        // entry size that is a multiple of 8 (checked in `new()`). `src` may be unaligned.
        unsafe {
            let dst = self.ring_cpu.add(self.wptr as usize) as *mut u64;
            for i in 0..core::mem::size_of::<U>() / 8 {
                ptr::write_volatile(dst.add(i), ptr::read_unaligned(src.add(i)));
            }
        }
        mem::sync();
        // SAFETY: `wptr_cpu` points to a live, aligned u32.
        unsafe { ptr::write_volatile(self.wptr_cpu, next_wptr) };
        self.wptr = next_wptr;
        self.wptr
    }

    /// Wait for a previously submitted message to be consumed by the firmware. See
    /// [`TxChannel::wait_for`].
    pub(crate) fn wait_for(&mut self, wptr: u32, timeout_ms: i64) -> Result {
        const MAX_FAST_POLL: i64 = 10;
        let start = Instant::<Monotonic>::now();
        let timeout_ms = timeout_ms.max(1);
        let timeout_fast = Delta::from_millis(timeout_ms.min(MAX_FAST_POLL));
        let timeout_slow = Delta::from_millis(timeout_ms);
        while start.elapsed() < timeout_fast {
            if self.rptr() == wptr {
                return Ok(());
            }
            mem::sync();
        }
        while start.elapsed() < timeout_slow {
            if self.rptr() == wptr {
                return Ok(());
            }
            fsleep(Delta::from_millis(5));
            mem::sync();
        }
        Err(ETIMEDOUT)
    }
}

/// The raw firmware descriptor type of a driver->FW channel. It depends on the firmware ABI:
/// a `{state, ring}` pair up to 13.x, a [`raw::ScratchRingDesc`] from 14.x.
pub(crate) trait TxChannelRaw {
    /// The descriptor type written into RuntimePointers.
    type Raw;
    /// Returns the descriptor.
    fn raw_desc(&self) -> Self::Raw;
}

/// Device Control channel for global device management commands.
#[versions(AGX)]
pub(crate) struct DeviceControlChannel {
    dev: AsahiDevRef,
    #[ver(V < V14_8_3)]
    ch: TxChannel<ChannelState, DeviceControlMsg::ver>,
    // 14.x: 4-pointer descriptor; the 0x38-byte entries stay in DRAM even on G15.
    #[ver(V >= V14_8_3)]
    ch: ScratchTxChannel<DeviceControlMsg::ver>,
}

#[versions(AGX)]
impl TxChannelRaw for DeviceControlChannel::ver {
    #[ver(V < V14_8_3)]
    type Raw = raw::ChannelRing<ChannelState, DeviceControlMsg::ver>;
    #[ver(V >= V14_8_3)]
    type Raw = raw::ScratchRingDesc;

    #[ver(V < V14_8_3)]
    fn raw_desc(&self) -> Self::Raw {
        self.ch.ring.to_raw()
    }
    #[ver(V >= V14_8_3)]
    fn raw_desc(&self) -> Self::Raw {
        self.ch.to_raw()
    }
}

#[versions(AGX)]
impl DeviceControlChannel::ver {
    const COMMAND_TIMEOUT_MS: i64 = 1000;

    /// Allocate a new Device Control channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<DeviceControlChannel::ver> {
        Ok(DeviceControlChannel::ver {
            dev: dev.into(),
            #[ver(V < V14_8_3)]
            ch: TxChannel::new(alloc, 0x100)?,
            // 256 x 0x38 entries (0x3800 bytes)
            #[ver(V >= V14_8_3)]
            ch: ScratchTxChannel::new(alloc, 0x100)?,
        })
    }

    /// Returns the raw ring descriptor to pass to firmware.
    pub(crate) fn to_raw(&self) -> <Self as TxChannelRaw>::Raw {
        self.raw_desc()
    }

    /// Moves the ring indices into the G15 Fender scratch SRAM. The DeviceControl
    /// ring entries stay in DRAM.
    #[allow(dead_code)] // Only the G15 (14.x) instantiation uses the SRAM placement.
    pub(crate) fn place_in_sram(&mut self, _sram: &mut ScratchRam) -> Result {
        #[ver(V >= V14_8_3)]
        return self.ch.place_in_sram(_sram, false);
        #[ver(V < V14_8_3)]
        return Err(ENODEV);
    }

    /// Submits a Device Control command.
    pub(crate) fn send(&mut self, msg: &DeviceControlMsg::ver) -> u32 {
        cls_dev_dbg!(DeviceControlCh, self.dev, "DeviceControl: {:?}\n", msg);
        // 14.x has no equivalent of the G13 Initialize/DestroyContext/GrowTVBAck commands.
        // Do not send them; the returned cookie is the current write pointer, so
        // `wait_for()` completes once the firmware has consumed everything queued before.
        // TODO: replace with the 14.x release-resource (0x11) / buffer-grow commands.
        #[ver(V >= V14_8_3)]
        if matches!(
            msg,
            DeviceControlMsg::ver::Initialize(_)
                | DeviceControlMsg::ver::DestroyContext { .. }
                | DeviceControlMsg::ver::GrowTVBAck { .. }
        ) {
            cls_dev_dbg!(
                DeviceControlCh,
                self.dev,
                "DeviceControl: not supported by this firmware, dropped\n"
            );
            return self.ch.put_cookie();
        }
        self.ch.put(msg)
    }

    /// Waits for a previously submitted Device Control command to complete.
    pub(crate) fn wait_for(&mut self, wptr: u32) -> Result {
        self.ch.wait_for(wptr, Self::COMMAND_TIMEOUT_MS)
    }

    /// Read-only snapshot of the 14.x ring index words (G15: in Fender SRAM). None before 14.x.
    pub(crate) fn indices(&self) -> Option<RingIndices> {
        #[ver(V >= V14_8_3)]
        return Some(self.ch.indices());
        #[ver(V < V14_8_3)]
        return None;
    }
}

/// Pipe channel to submit WorkQueue execution requests.
#[versions(AGX)]
pub(crate) struct PipeChannel {
    dev: AsahiDevRef,
    #[ver(V < V14_8_3)]
    ch: TxChannel<ChannelState, PipeMsg::ver>,
    // 14.x: 4-pointer descriptor, 0x18-byte entries; ring and indices in SRAM on G15.
    #[ver(V >= V14_8_3)]
    ch: ScratchTxChannel<PipeMsg::ver>,
}

#[versions(AGX)]
impl TxChannelRaw for PipeChannel::ver {
    #[ver(V < V14_8_3)]
    type Raw = raw::ChannelRing<ChannelState, PipeMsg::ver>;
    #[ver(V >= V14_8_3)]
    type Raw = raw::ScratchRingDesc;

    #[ver(V < V14_8_3)]
    fn raw_desc(&self) -> Self::Raw {
        self.ch.ring.to_raw()
    }
    #[ver(V >= V14_8_3)]
    fn raw_desc(&self) -> Self::Raw {
        self.ch.to_raw()
    }
}

#[versions(AGX)]
impl PipeChannel::ver {
    /// Allocate a new Pipe submission channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<PipeChannel::ver> {
        Ok(PipeChannel::ver {
            dev: dev.into(),
            #[ver(V < V14_8_3)]
            ch: TxChannel::new(alloc, 0x100)?,
            // 256 x 0x18 entries = 0x1800 bytes per pipe ring
            #[ver(V >= V14_8_3)]
            ch: ScratchTxChannel::new(alloc, 0x100)?,
        })
    }

    /// Returns the raw ring descriptor to pass to firmware.
    pub(crate) fn to_raw(&self) -> <Self as TxChannelRaw>::Raw {
        self.raw_desc()
    }

    /// Moves the ring and its indices into the G15 Fender scratch SRAM.
    #[allow(dead_code)] // Only the G15 (14.x) instantiation uses the SRAM placement.
    pub(crate) fn place_in_sram(&mut self, _sram: &mut ScratchRam) -> Result {
        #[ver(V >= V14_8_3)]
        return self.ch.place_in_sram(_sram, true);
        #[ver(V < V14_8_3)]
        return Err(ENODEV);
    }

    /// Submits a Pipe kick command to the firmware.
    pub(crate) fn send(&mut self, msg: &PipeMsg::ver) {
        cls_dev_dbg!(PipeCh, self.dev, "Pipe: {:?}\n", msg);
        self.ch.put(msg);
    }

    /// Read-only snapshot of the 14.x ring index words (G15: in Fender SRAM). None before 14.x.
    pub(crate) fn indices(&self) -> Option<RingIndices> {
        #[ver(V >= V14_8_3)]
        return Some(self.ch.indices());
        #[ver(V < V14_8_3)]
        return None;
    }
}

/// Ring storage of the Firmware Control channel.
enum FwCtlRing {
    /// 0x14-byte entries (firmware up to 13.x).
    Legacy(TxChannel<FwCtlChannelState, FwCtlMsg>),
    /// 0x18-byte entries (14.x).
    Padded(TxChannel<FwCtlChannelState, FwCtlMsgPadded>),
}

/// Firmware Control channel, used for secure cache flush requests.
pub(crate) struct FwCtlChannel {
    dev: AsahiDevRef,
    ch: FwCtlRing,
}

impl FwCtlChannel {
    const COMMAND_TIMEOUT_MS: i64 = 1000;

    /// Allocate a new Firmware Control channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<FwCtlChannel> {
        Ok(FwCtlChannel {
            dev: dev.into(),
            ch: FwCtlRing::Legacy(TxChannel::<FwCtlChannelState, FwCtlMsg>::new_uncached(
                alloc, 0x100,
            )?),
        })
    }

    /// Allocate a new Firmware Control channel with the 14.x 0x18-byte entry stride. The state
    /// block keeps the G13 `FwCtlChannelState` layout.
    #[allow(dead_code)] // Only the G15 (14.x) instantiation uses the padded channel.
    pub(crate) fn new_padded(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<FwCtlChannel> {
        Ok(FwCtlChannel {
            dev: dev.into(),
            ch: FwCtlRing::Padded(
                TxChannel::<FwCtlChannelState, FwCtlMsgPadded>::new_uncached(alloc, 0x100)?,
            ),
        })
    }

    /// Returns the raw `ChannelRing` structure to pass to firmware.
    pub(crate) fn to_raw(&self) -> raw::ChannelRing<FwCtlChannelState, FwCtlMsg> {
        match &self.ch {
            FwCtlRing::Legacy(ch) => ch.ring.to_raw(),
            FwCtlRing::Padded(ch) => {
                let r = ch.ring.to_raw();
                raw::ChannelRing {
                    state: r.state,
                    // SAFETY: GpuWeakPointer is a packed (NonZeroU64, PhantomData) for every
                    // target type; only the pointee type the firmware never sees changes.
                    ring: r.ring.map(|p| unsafe {
                        core::mem::transmute::<
                            GpuWeakPointer<[FwCtlMsgPadded]>,
                            GpuWeakPointer<[FwCtlMsg]>,
                        >(p)
                    }),
                }
            }
        }
    }

    /// Submits a Firmware Control command to the firmware.
    pub(crate) fn send(&mut self, msg: &FwCtlMsg) -> u32 {
        cls_dev_dbg!(FwCtlCh, self.dev, "FwCtl: {:?}\n", msg);
        match &mut self.ch {
            FwCtlRing::Legacy(ch) => ch.put(msg),
            FwCtlRing::Padded(ch) => ch.put(&FwCtlMsgPadded {
                msg: *msg,
                __pad: 0,
            }),
        }
    }

    /// Waits for a previously submitted Firmware Control command to complete.
    pub(crate) fn wait_for(&mut self, wptr: u32) -> Result {
        match &mut self.ch {
            FwCtlRing::Legacy(ch) => ch.wait_for(wptr, Self::COMMAND_TIMEOUT_MS),
            FwCtlRing::Padded(ch) => ch.wait_for(wptr, Self::COMMAND_TIMEOUT_MS),
        }
    }
}

/// Event channel, used to notify the driver of command completions, GPU faults and errors, and
/// other events.
#[versions(AGX)]
pub(crate) struct EventChannel {
    dev: AsahiDevRef,
    ch: RxChannel<ChannelState, RawEventMsg>,
    ev_mgr: Arc<event::EventManager>,
    buf_mgr: buffer::BufferManager::ver,
    gpu: Option<Arc<dyn gpu::GpuManager>>,
    /// Number of messages received on this channel (diagnostics).
    received: u64,
}

#[versions(AGX)]
impl EventChannel::ver {
    /// Allocate a new Event channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
        ev_mgr: Arc<event::EventManager>,
        buf_mgr: buffer::BufferManager::ver,
    ) -> Result<EventChannel::ver> {
        Ok(EventChannel::ver {
            dev: dev.into(),
            ch: RxChannel::<ChannelState, RawEventMsg>::new(alloc, 0x100)?,
            ev_mgr,
            buf_mgr,
            gpu: None,
            received: 0,
        })
    }

    /// Returns the number of messages received on this channel so far.
    pub(crate) fn received(&self) -> u64 {
        self.received
    }

    /// Read-only (firmware wptr, host rptr) per sub-channel (G15 bring-up diagnostics).
    pub(crate) fn ring_snapshot(&self) -> KVec<(u32, u32)> {
        self.ch.snapshot()
    }

    /// Registers the managing `Gpu` instance that will handle events on this channel.
    pub(crate) fn set_manager(&mut self, gpu: Arc<dyn gpu::GpuManager>) {
        self.gpu = Some(gpu);
    }

    /// Returns the raw `ChannelRing` structure to pass to firmware.
    pub(crate) fn to_raw(&self) -> raw::ChannelRing<ChannelState, RawEventMsg> {
        self.ch.ring.to_raw()
    }

    /// Polls for new Event messages on this ring.
    pub(crate) fn poll(&mut self) {
        #[ver(V >= V14_8_3)]
        self.poll_14x();
        #[ver(V < V14_8_3)]
        while let Some(msg) = self.ch.get(0) {
            self.received += 1;
            // SAFETY: The raw view is always valid for all bit patterns.
            let tag = unsafe { msg.raw.0 };
            match tag {
                0..=EVENT_MAX => {
                    // SAFETY: Since we have checked the tag to be in range,
                    // accessing the enum view is valid.
                    let msg = unsafe { msg.msg };

                    cls_dev_dbg!(EventCh, self.dev, "Event: {:?}\n", msg);
                    match msg {
                        EventMsg::Fault => match self.gpu.as_ref() {
                            Some(gpu) => gpu.handle_fault(),
                            None => {
                                dev_crit!(
                                    self.dev.as_ref(),
                                    "EventChannel: No GPU manager available!\n"
                                )
                            }
                        },
                        EventMsg::Timeout {
                            counter,
                            unk_8,
                            event_slot,
                        } => match self.gpu.as_ref() {
                            Some(gpu) => gpu.handle_timeout(counter, event_slot, unk_8),
                            None => {
                                dev_crit!(
                                    self.dev.as_ref(),
                                    "EventChannel: No GPU manager available!\n"
                                )
                            }
                        },
                        EventMsg::Flag { firing, .. } => {
                            for (i, flags) in firing.iter().enumerate() {
                                for j in 0..32 {
                                    if flags & (1u32 << j) != 0 {
                                        self.ev_mgr.signal((i * 32 + j) as u32);
                                    }
                                }
                            }
                        }
                        EventMsg::GrowTVB {
                            vm_slot,
                            buffer_slot,
                            counter,
                        } => match self.gpu.as_ref() {
                            Some(gpu) => {
                                self.buf_mgr.grow(buffer_slot);
                                gpu.ack_grow(buffer_slot, vm_slot, counter);
                            }
                            None => {
                                dev_crit!(
                                    self.dev.as_ref(),
                                    "EventChannel: No GPU manager available!\n"
                                )
                            }
                        },
                        EventMsg::ChannelError {
                            error_type,
                            pipe_type,
                            event_slot,
                            event_value,
                        } => match self.gpu.as_ref() {
                            Some(gpu) => {
                                let error_type = match error_type {
                                    0 => ChannelErrorType::MemoryError,
                                    1 => ChannelErrorType::DMKill,
                                    2 => ChannelErrorType::Aborted,
                                    3 => ChannelErrorType::Unk3,
                                    a => ChannelErrorType::Unknown(a),
                                };
                                gpu.handle_channel_error(
                                    error_type,
                                    pipe_type,
                                    event_slot,
                                    event_value,
                                );
                            }
                            None => {
                                dev_crit!(
                                    self.dev.as_ref(),
                                    "EventChannel: No GPU manager available!\n"
                                )
                            }
                        },
                        msg => {
                            dev_crit!(self.dev.as_ref(), "Unknown event message: {:?}\n", msg);
                        }
                    }
                }
                _ => {
                    // SAFETY: The raw view is always valid for all bit patterns.
                    dev_warn!(self.dev.as_ref(), "Unknown event message: {:?}\n", unsafe {
                        msg.raw
                    });
                }
            }
        }
    }

    // Polls for new Event messages with the 14.x numbering (types 0..15).
    // (Plain comment: the versions macro needs `fn` right after a gating #[ver].)
    #[ver(V >= V14_8_3)]
    fn poll_14x(&mut self) {
        while let Some(msg) = self.ch.get(0) {
            self.received += 1;
            // SAFETY: The raw view is always valid for all bit patterns.
            let tag = unsafe { msg.raw.0 };
            // G15 bring-up: log every incoming event (bounded).
            if g15_rx_log_budget() {
                // SAFETY: The raw view is always valid for all bit patterns.
                dev_info!(
                    self.dev.as_ref(),
                    "G15 event #{}: raw {:x?}\n",
                    self.received,
                    unsafe { msg.raw }
                );
            }
            if tag > EVENT_MAX_G15 {
                // SAFETY: The raw view is always valid for all bit patterns.
                dev_warn!(self.dev.as_ref(), "Unknown event message: {:?}\n", unsafe {
                    msg.raw
                });
                continue;
            }
            // SAFETY: Since we have checked the tag to be in range, accessing the enum view is
            // valid.
            let msg = unsafe { msg.msg_g15 };
            cls_dev_dbg!(EventCh, self.dev, "Event: {:?}\n", msg);

            let gpu = match self.gpu.as_ref() {
                Some(gpu) => gpu,
                None => {
                    dev_crit!(
                        self.dev.as_ref(),
                        "EventChannel: No GPU manager available!\n"
                    );
                    continue;
                }
            };

            match msg {
                EventMsgG15::Flag { firing, .. } => {
                    for (i, flags) in firing.iter().enumerate() {
                        for j in 0..32 {
                            if flags & (1u32 << j) != 0 {
                                self.ev_mgr.signal((i * 32 + j) as u32);
                            }
                        }
                    }
                }
                // Faults arrive as recovery packets on 14.x.
                // TODO: decode StatusBlock+0x44a0 instead of only the fault registers.
                EventMsgG15::RecoveryPacket(_) => gpu.handle_fault(),
                // TODO: the type-7 payload is only inferred to be the G13 GrowTVB layout, and
                // 14.x has no GrowTVBAck (the ack is dropped by DeviceControlChannel::send).
                // BufferManager::grow() indexes a 127-entry table unchecked, so reject
                // out-of-range slots instead of panicking, and log every type-7 event.
                EventMsgG15::GrowTVB {
                    vm_slot,
                    buffer_slot,
                    counter,
                } => {
                    dev_warn!(
                        self.dev.as_ref(),
                        "G15 event type 7 (GrowTVB?): vm_slot={} buffer_slot={} counter={}\n",
                        vm_slot,
                        buffer_slot,
                        counter
                    );
                    if buffer_slot < buffer::NUM_BUFFERS {
                        self.buf_mgr.grow(buffer_slot);
                        gpu.ack_grow(buffer_slot, vm_slot, counter);
                    }
                }
                EventMsgG15::ChannelError {
                    error_type,
                    pipe_type,
                    event_slot,
                    event_value,
                } => {
                    let error_type = match error_type {
                        0 => ChannelErrorType::MemoryError,
                        1 => ChannelErrorType::DMKill,
                        2 => ChannelErrorType::Aborted,
                        3 => ChannelErrorType::Unk3,
                        a => ChannelErrorType::Unknown(a),
                    };
                    gpu.handle_channel_error(error_type, pipe_type, event_slot, event_value);
                }
                // Not needed by the host for job completion.
                EventMsgG15::Unk2(_)
                | EventMsgG15::Unk3(_)
                | EventMsgG15::Unk5(_)
                | EventMsgG15::Unk6(_)
                | EventMsgG15::Unk12(_) => (),
                // Controller events and types 9-15 (UMA grow, CLPC, ...) have no handler yet.
                msg => {
                    cls_dev_dbg!(EventCh, self.dev, "Unhandled event message: {:?}\n", msg);
                }
            }
        }
    }
}

/// Firmware Log channel. This one is pretty special, since it has 6 sub-channels (for different log
/// levels), and it also uses a side buffer to actually hold the log messages, only passing around
/// pointers in the main buffer.
pub(crate) struct FwLogChannel {
    dev: AsahiDevRef,
    ch: RxChannel<FwLogChannelState, RawFwLogMsg>,
    payload_buf: GpuArray<RawFwLogPayloadMsg>,
    /// Number of ring entries received (diagnostics).
    received: u64,
    /// G15 bring-up: log every message at info level (bounded by the RX log budget).
    log_all_info: bool,
}

impl FwLogChannel {
    const RING_SIZE: usize = 0x100;
    const BUF_SIZE: usize = 0x100;

    /// Allocate a new Firmware Log channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<FwLogChannel> {
        Ok(FwLogChannel {
            dev: dev.into(),
            ch: RxChannel::<FwLogChannelState, RawFwLogMsg>::new(alloc, Self::RING_SIZE)?,
            payload_buf: alloc
                .shared
                .array_empty(Self::BUF_SIZE * FwLogChannelState::SUB_CHANNELS)?,
            received: 0,
            log_all_info: false,
        })
    }

    /// Log every received FWLog message at info level (G15 bring-up, bounded).
    pub(crate) fn set_log_all_info(&mut self, on: bool) {
        self.log_all_info = on;
    }

    /// Returns the number of ring entries received so far.
    pub(crate) fn received(&self) -> u64 {
        self.received
    }

    /// Read-only (firmware wptr, host rptr) per sub-channel (G15 bring-up diagnostics).
    pub(crate) fn ring_snapshot(&self) -> KVec<(u32, u32)> {
        self.ch.snapshot()
    }

    /// Returns the raw `ChannelRing` structure to pass to firmware.
    pub(crate) fn to_raw(&self) -> raw::ChannelRing<FwLogChannelState, RawFwLogMsg> {
        self.ch.ring.to_raw()
    }

    /// Returns the GPU pointers to the firmware log payload buffer.
    pub(crate) fn get_buf(&self) -> GpuWeakPointer<[RawFwLogPayloadMsg]> {
        self.payload_buf.weak_pointer()
    }

    /// Polls for new log messages on all sub-rings.
    pub(crate) fn poll(&mut self) {
        for i in 0..=FwLogChannelState::SUB_CHANNELS - 1 {
            while let Some(msg) = self.ch.peek(i) {
                self.received += 1;
                cls_dev_dbg!(FwLogCh, self.dev, "FwLog{}: {:?}\n", i, msg);
                if msg.msg_type != 2 {
                    dev_warn!(self.dev.as_ref(), "Unknown FWLog{} message: {:?}\n", i, msg);
                    self.ch.get(i);
                    continue;
                }
                if msg.msg_index.0 as usize >= Self::BUF_SIZE {
                    dev_warn!(
                        self.dev.as_ref(),
                        "FWLog{} message index out of bounds: {:?}\n",
                        i,
                        msg
                    );
                    self.ch.get(i);
                    continue;
                }
                let index = Self::BUF_SIZE * i + msg.msg_index.0 as usize;
                let payload = &self.payload_buf.as_slice()[index];
                if payload.msg_type != 3 {
                    dev_warn!(
                        self.dev.as_ref(),
                        "Unknown FWLog{} payload: {:?}\n",
                        i,
                        payload
                    );
                    self.ch.get(i);
                    continue;
                }
                let msg = if let Some(end) = payload.msg.iter().position(|&r| r == 0) {
                    CStr::from_bytes_with_nul(&(*payload.msg)[..end + 1])
                        .unwrap_or(c_str!("cstr_err"))
                } else {
                    dev_warn!(
                        self.dev.as_ref(),
                        "FWLog{} payload not NUL-terminated: {:?}\n",
                        i,
                        payload
                    );
                    self.ch.get(i);
                    continue;
                };
                match i {
                    0 if self.log_all_info && g15_rx_log_budget() => {
                        dev_info!(self.dev.as_ref(), "FWLog: {}\n", msg)
                    }
                    0 => dev_dbg!(self.dev.as_ref(), "FWLog: {}\n", msg),
                    1 => dev_info!(self.dev.as_ref(), "FWLog: {}\n", msg),
                    2 => dev_notice!(self.dev.as_ref(), "FWLog: {}\n", msg),
                    3 => dev_warn!(self.dev.as_ref(), "FWLog: {}\n", msg),
                    4 => dev_err!(self.dev.as_ref(), "FWLog: {}\n", msg),
                    5 => dev_crit!(self.dev.as_ref(), "FWLog: {}\n", msg),
                    _ => (),
                };
                self.ch.get(i);
            }
        }
    }
}

pub(crate) struct KTraceChannel {
    dev: AsahiDevRef,
    ch: RxChannel<ChannelState, RawKTraceMsg>,
}

/// KTrace channel, used to receive detailed execution trace markers from the firmware.
/// We currently disable this in initdata, so no messages are expected here at this time.
impl KTraceChannel {
    /// Allocate a new KTrace channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<KTraceChannel> {
        Ok(KTraceChannel {
            dev: dev.into(),
            ch: RxChannel::<ChannelState, RawKTraceMsg>::new(alloc, 0x200)?,
        })
    }

    /// Returns the raw `ChannelRing` structure to pass to firmware.
    pub(crate) fn to_raw(&self) -> raw::ChannelRing<ChannelState, RawKTraceMsg> {
        self.ch.ring.to_raw()
    }

    /// Polls for new KTrace messages on this ring.
    pub(crate) fn poll(&mut self) {
        while let Some(msg) = self.ch.get(0) {
            cls_dev_dbg!(KTraceCh, self.dev, "KTrace: {:?}\n", msg);
        }
    }
}

/// Statistics channel, reporting power-related statistics to the driver.
/// Not really implemented other than debug logs yet...
#[versions(AGX)]
pub(crate) struct StatsChannel {
    dev: AsahiDevRef,
    ch: RxChannel<ChannelState, RawStatsMsg::ver>,
    /// Counts of message types this driver does not decode, by type (the last slot collects
    /// every type above it).
    unknown: [u64; 17],
}

#[versions(AGX)]
impl StatsChannel::ver {
    /// Allocate a new Statistics channel.
    pub(crate) fn new(
        dev: &AsahiDevice,
        alloc: &mut gpu::KernelAllocators,
    ) -> Result<StatsChannel::ver> {
        Ok(StatsChannel::ver {
            dev: dev.into(),
            ch: RxChannel::<ChannelState, RawStatsMsg::ver>::new(alloc, 0x100)?,
            unknown: [0; 17],
        })
    }

    /// Returns the raw `ChannelRing` structure to pass to firmware.
    pub(crate) fn to_raw(&self) -> raw::ChannelRing<ChannelState, RawStatsMsg::ver> {
        self.ch.ring.to_raw()
    }

    /// Polls for new statistics messages on this ring.
    pub(crate) fn poll(&mut self) {
        while let Some(msg) = self.ch.get(0) {
            // SAFETY: The raw view is always valid for all bit patterns.
            let tag = unsafe { msg.raw.0 };
            match tag {
                0..=STATS_MAX::ver => {
                    // SAFETY: Since we have checked the tag to be in range,
                    // accessing the enum view is valid.
                    let msg = unsafe { msg.msg };
                    cls_dev_dbg!(StatsCh, self.dev, "Stats: {:?}\n", msg);
                }
                _ => {
                    // Some firmware sends periodic records this driver does not decode (type 15
                    // arrives about 250 times per second on G15). Count them: log the first one
                    // of each type, then one summary line each time the count doubles from 1024.
                    let slot = (tag as usize).min(self.unknown.len() - 1);
                    self.unknown[slot] += 1;
                    let count = self.unknown[slot];
                    if count == 1 {
                        // SAFETY: The raw view is always valid for all bit patterns.
                        dev_info!(
                            self.dev.as_ref(),
                            "Unknown stats message type {} (counted, not logged again): {:?}\n",
                            tag,
                            unsafe { msg.raw }
                        );
                    } else if count >= 1024 && count.is_power_of_two() {
                        dev_info!(
                            self.dev.as_ref(),
                            "{} unknown stats messages of type {} so far\n",
                            count,
                            tag
                        );
                    }
                }
            }
        }
    }
}
