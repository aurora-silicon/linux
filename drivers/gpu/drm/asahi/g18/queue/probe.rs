// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! J873/26A428 queue registration and one bounded compute submission.
//!
//! Native kernelcache: AGXChannel::resetChannelState 0xfffffe0008bdd178;
//! AGXCLChannelSKSM::submitBuffer 0xfffffe0008be3ddc..0x8be3fcc.
//! GFX b000: dispatcher 0x1267c, tag15 target 0x12824, table 0xb90b8.
//! M6 adds 12 bytes at config +0x6e; never use M5's 0x86-byte packet here.

use crate::g18::buffer::object::MappedObject;
use crate::g18::buffer::pool::G18UmaPoolBinding;
use crate::g18::runtime::compute::G18PreparedComputeCommand;
use crate::{driver::AsahiDrmDevice, mmu};
use core::sync::atomic::{fence, Ordering};
use kernel::{c_str, io, prelude::*};

const KERNEL_BASE: u64 = 0xffff_fc20_0000_0000;
const GRAPH_BASE: u64 = KERNEL_BASE + 0xc000_0000;
const ENTRY_HIGH: u64 = KERNEL_BASE + 0x39_0000;
const ENTRY_LOW: u64 = 0x70_005f_0000;
const ENTRY_SIZE: usize = 0x20000; // b000 0x1257c/0x125a0: 256 * 0x200
pub(crate) const QUEUE_ID: u8 = 11;
const POINTERS: usize = 0x100;
const JOB_LIST: usize = 0x200;
const CONTROL: usize = 0x300;
const ITEMS: usize = 0x800;
const CAPACITY: u32 = 0x100;
const CONFIG: usize = 0x1800;
const DATA_SIZE: usize = 0xbc000;
const SLOT: usize = 0xb90b8 - 0x50000 + QUEUE_ID as usize * 0x28;
const SCHEDULER: usize = 0xb7468 - 0x50000 + QUEUE_ID as usize * 8;
const POOL_SLOT: usize = 0x5f660 - 0x50000; // native slot stride 0x100
const QUEUE_POOLS: usize = 0x6f660 - 0x50000 + QUEUE_ID as usize * 0x38;

/// Reserved GFX DATA, never written by the host. No native snapshot pointers
/// are used: the current DT reservation is the only physical address source.
struct FirmwareDataView {
    map: io::mem::Mem,
    cache_line: usize,
}
// SAFETY: The fixed reserved RAM mapping is observed only through bounded
// volatile reads; no host write or ownership transfer is possible.
unsafe impl Send for FirmwareDataView {}
// SAFETY: Concurrent read-only observations cannot mutate host-owned state.
unsafe impl Sync for FirmwareDataView {}

impl FirmwareDataView {
    fn new(dev: &AsahiDrmDevice) -> Result<Self> {
        let resource = dev.as_ref().as_ref().of_node().ok_or(EINVAL)?
            .reserved_mem_region_to_resource_byname(c_str!("gfx-data"))?;
        if resource.size() != DATA_SIZE as u64 || resource.start() < 0x100_0000_0000
            || resource.start() & 0x3fff != 0
            || resource.start().checked_add(resource.size()).ok_or(EOVERFLOW)? > 0x104_0000_0000
        {
            return Err(EINVAL);
        }
        let ctr: u64;
        // SAFETY: Architectural read-only cache geometry register.
        unsafe { core::arch::asm!("mrs {ctr}, ctr_el0", ctr = out(reg) ctr,
            options(nomem, nostack, preserves_flags)) };
        let cache_line = 4usize << ((ctr >> 16) & 0xf);
        if cache_line > 0x4000 || (SLOT + 40 + cache_line - 1) & !(cache_line - 1) > DATA_SIZE {
            return Err(EINVAL);
        }
        // SAFETY: WB matches reserved System RAM; the host never dirties it.
        let map = unsafe { io::mem::Mem::try_new(resource, io::mem::MemFlag::WB.into()) }?;
        Ok(Self { map, cache_line })
    }

    fn words<const N: usize>(&self, offset: usize) -> [u64; N] {
        // The only callers are the fixed slot and scheduler extents. Both
        // are below the constructor-checked slot end inside reserved DATA.
        let start = offset & !(self.cache_line - 1);
        let end = (offset + N * 8 + self.cache_line - 1) & !(self.cache_line - 1);
        for offset in (start..end).step_by(self.cache_line) {
            // SAFETY: Constructor bounds the whole covering cache lines.
            // Invalidate only: cleaning stale data would corrupt firmware.
            unsafe { core::arch::asm!("dc ivac, {addr}",
                addr = in(reg) self.map.ptr().add(offset), options(nostack, preserves_flags)) };
        }
        // SAFETY: Complete cache invalidation before observing firmware stores.
        unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) };
        let mut out = [0; N];
        for (index, word) in out.iter_mut().enumerate() {
            // SAFETY: Aligned reads wholly inside the constructor-checked slot.
            *word = unsafe { core::ptr::read_volatile(
                self.map.ptr().add(offset + index * 8).cast::<u64>()) };
        }
        fence(Ordering::Acquire);
        out
    }
}

pub(crate) struct G18QueueProbe {
    _entry_alias: mmu::KernelMapping,
    _entry: MappedObject,
    graph: MappedObject,
    firmware: FirmwareDataView,
    compute_prepared: bool,
    compute_generation: u32,
    pool_slot: u32,
}

fn put32(out: &mut [u8], offset: usize, value: u32) {
    out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put64(out: &mut [u8], offset: usize, value: u64) {
    out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

impl G18QueueProbe {
    pub(crate) fn new(dev: &AsahiDrmDevice, uat: &mmu::Uat, bind_scheduler: bool) -> Result<Self> {
        if !uat.is_t8152_geometry() { return Err(ENODEV); }
        let mut entry = MappedObject::new_wc_at(dev, uat.kernel_vm(), ENTRY_HIGH,
            ENTRY_SIZE, mmu::PROT_FW_PRIV_RW)?;
        let entry_alias = entry.map_alias_at(uat.kernel_lower_vm(), ENTRY_LOW,
            mmu::PROT_GPU_SHARED_RW)?;
        for offset in (0..ENTRY_SIZE).step_by(mmu::UAT_PGSZ) {
            if uat.kernel_vm().translate_iova(ENTRY_HIGH + offset as u64)?
                != uat.kernel_lower_vm().translate_iova(ENTRY_LOW + offset as u64)? {
                return Err(EFAULT);
            }
        }
        let mut graph = MappedObject::new_wc_at(dev, uat.kernel_vm(), GRAPH_BASE,
            0x4000, mmu::PROT_FW_PRIV_RW)?;
        graph.with_bytes_mut(|out| {
            // M6 channel state: resetChannelState zeros 0x80 bytes. The
            // command processor only needs the pointers and ring capacity
            // for this tag15-only experiment; retain native reset sentinels.
            put64(out, 0, GRAPH_BASE + POINTERS as u64);
            put64(out, 8, GRAPH_BASE + ITEMS as u64);
            put64(out, 0x10, GRAPH_BASE + JOB_LIST as u64);
            put32(out, 0x24, u32::MAX);
            put32(out, 0x28, 4);
            put32(out, 0x40, u32::MAX);
            put32(out, 0x44, 0);
            put64(out, 0x70, GRAPH_BASE + CONTROL as u64);
            put32(out, POINTERS + 0x50, u32::MAX);
            put32(out, POINTERS + 0x60, CAPACITY);
            put64(out, JOB_LIST + 8, GRAPH_BASE + JOB_LIST as u64);
            if bind_scheduler {
                // 26A428 AGXCommandQueueReclaimableState::init at
                // 0xfffffe0008bf9014. Both indices start unallocated.
                out[CONTROL..CONTROL + 2].fill(0xff);
                out[CONTROL + 5] = 1;
                out[CONTROL + 0x33] = 0xff;
                // b000 0xb234 sets2 on this fresh process-slot path.
                out[CONTROL + 0x26] = 2;
            }

            // b000 0x12824 requests 0x98 bytes. Unwritten trailing padding
            // is zero. UMA/PB pool bindings and the new +0x6e/+0x72 grow
            // pair are disabled. UMA +0x3e is a FList generation identity,
            // not an event pointer; +0x46 selects its firmware pool slot.
            let config = &mut out[CONFIG..CONFIG + 0x98];
            put32(config, 0, 15);
            put64(config, 8, ENTRY_LOW);
            put64(config, 0x10, ENTRY_HIGH);
            config[0x18..0x1a].copy_from_slice(&u16::from(QUEUE_ID).to_le_bytes());
            for (offset, value) in [(0x1a, 1), (0x1e, 2), (0x22, 2), (0x32, 3), (0x5e, 2)] {
                put32(config, offset, value);
            }
            put64(config, 0x3e, u64::MAX);
            put32(config, 0x46, u32::MAX);
            put64(config, 0x4a, GRAPH_BASE + CONTROL as u64);
            config[0x82..0x92].fill(0xff);
            put64(out, ITEMS, GRAPH_BASE + CONFIG as u64);
            put32(out, POINTERS + 0x40, 1);
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        Ok(Self { _entry_alias: entry_alias, _entry: entry, graph,
            firmware: FirmwareDataView::new(dev)?, compute_prepared: false,
            compute_generation: 0, pool_slot: 0 })
    }

    pub(crate) fn queue_address(&self) -> u64 { GRAPH_BASE }

    /// The addition fixture owns context3. In DRM mode the retained graphics
    /// owner already holds it, so compute must install the context it actually
    /// acquired before its first descriptor is queued. The later context-update
    /// command follows that descriptor and cannot repair the initial binding.
    pub(crate) fn prepare_initial_context(&mut self, context: u32) -> Result {
        if !(3..64).contains(&context) || self.counters()? != [0, 0, 1]
            || self.slot() != [0; 5] { return Err(EBUSY); }
        self.graph.with_bytes_mut(|out| {
            put32(out, CONFIG + 0x32, context);
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    pub(crate) fn counters(&mut self) -> Result<[u32; 3]> {
        Ok([self.graph.read_u32(GRAPH_BASE + POINTERS as u64)?,
            self.graph.read_u32(GRAPH_BASE + POINTERS as u64 + 0x30)?,
            self.graph.read_u32(GRAPH_BASE + POINTERS as u64 + 0x40)?])
    }

    pub(crate) fn slot(&self) -> [u64; 5] { self.firmware.words(SLOT) }

    pub(crate) fn scheduler_status(&mut self) -> Result<[u64; 4]> {
        Ok([self.firmware.words::<1>(SCHEDULER)[0],
            self.graph.read_u64(GRAPH_BASE + CONTROL as u64)?,
            self.graph.read_u64(GRAPH_BASE + CONTROL as u64 + 0x12)?,
            self.graph.read_u64(GRAPH_BASE + CONTROL as u64 + 0x1a)?])
    }

    pub(crate) fn prepare_scheduler_bind(&mut self) -> Result {
        if self.counters()? != [1; 3] || !Self::installed(self.slot())
            || self.scheduler_status()? != [0, 0x1000000ffff, 0, 0] {
            return Err(EBUSY);
        }
        self.graph.with_bytes_mut(|out| {
            if out[CONFIG + 0x100..CONFIG + 0x198].iter().any(|x| *x != 0) {
                return Err(EBUSY);
            }
            out.copy_within(CONFIG..CONFIG + 0x98, CONFIG + 0x100);
            let config = &mut out[CONFIG + 0x100..CONFIG + 0x198];
            put32(config, 0x1a, 0); // already installed
            put32(config, 0x52, 1); // bind scheduler
            put64(out, ITEMS + 8, GRAPH_BASE + CONFIG as u64 + 0x100);
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        self.graph.write_u32(GRAPH_BASE + POINTERS as u64 + 0x40, 2)?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    pub(crate) fn scheduler_bound([pointer, header, low, high]: [u64; 4]) -> bool {
        pointer == GRAPH_BASE + CONTROL as u64 && header & 0xff != 0xff
            && (header >> 8) & 0xff != 0xff
            && (header >> 16) & 0xffff_ffff == 0x0104_0204
            && low == 1u64 << QUEUE_ID && high == 0
    }

    /// Observe both directions of firmware-owned pool membership, in addition
    /// to the state/generation tuple. Retirement alone is not a binding proof.
    pub(crate) fn pool_status(&self) -> [u64; 9] {
        let slot = POOL_SLOT + self.pool_slot as usize * 0x100;
        let header = self.firmware.words::<3>(slot);
        let members = self.firmware.words::<2>(slot + 0x78);
        let flags = self.firmware.words::<1>(slot + 0xe0);
        let growth = self.firmware.words::<2>(slot + 0xf0);
        let queues = self.firmware.words::<1>(QUEUE_POOLS);
        [header[0], header[1], header[2], members[0], members[1], flags[0],
         growth[0], growth[1], queues[0]]
    }

    pub(crate) fn prepare_pool_bind(&mut self, binding: G18UmaPoolBinding) -> Result {
        if binding.slot > 1 { return Err(EINVAL); }
        self.pool_slot = binding.slot;
        let before = self.pool_status();
        if self.counters()? != [2; 3] || !Self::installed(self.slot())
            || !Self::scheduler_bound(self.scheduler_status()?)
            || binding.slot > 1 || binding.generation != 0
            || binding.state < KERNEL_BASE || binding.state & 0x3fff != 0
            || before[0] != 0 || before[3..5] != [0; 2]
            || before[6..9] != [0; 3] {
            return Err(EBUSY);
        }
        self.graph.with_bytes_mut(|out| {
            if out[CONFIG + 0x200..CONFIG + 0x298].iter().any(|x| *x != 0)
                || out[CONFIG + 0x136..CONFIG + 0x13e].iter().any(|x| *x != 0)
                || out[CONFIG + 0x13e..CONFIG + 0x14a].iter().any(|x| *x != 0xff) {
                return Err(EBUSY);
            }
            out.copy_within(CONFIG + 0x100..CONFIG + 0x198, CONFIG + 0x200);
            let config = &mut out[CONFIG + 0x200..CONFIG + 0x298];
            put32(config, 0x52, 0); // scheduler is already bound
            put64(config, 0x36, binding.state);
            put64(config, 0x3e, binding.generation);
            put32(config, 0x46, binding.slot);
            put64(out, ITEMS + 16, GRAPH_BASE + CONFIG as u64 + 0x200);
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        self.graph.write_u32(GRAPH_BASE + POINTERS as u64 + 0x40, 3)?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    pub(crate) fn pool_bound(status: [u64; 9], binding: G18UmaPoolBinding) -> bool {
        Self::pool_bound_for_context(status, binding, 3)
    }

    pub(crate) fn pool_bound_for_context(status: [u64; 9], binding: G18UmaPoolBinding,
        context: u32) -> bool {
        (3..64).contains(&context)
            && status == [binding.state, binding.generation, (2u64 << 32) | u64::from(context),
                   1u64 << QUEUE_ID, 0, 1, 0, 0, 1u64 << binding.slot]
    }

    /// Store an unpublished SKSM entry and its three inner commands. The
    /// original HAL400 encoder 8c4a290, dependency writer 8c4a5a4 and timestamp
    /// converter 8c5620c agree byte-for-byte with the shared M5 serializer in
    /// this bounded domain (audit-g18-sksm-native.py). The tag15 remains the
    /// M6 0x98-byte form; never substitute the shorter M5 configuration.
    pub(crate) fn prepare_compute_job(&mut self, command: G18PreparedComputeCommand,
                                     pool: G18UmaPoolBinding) -> Result {
        self.prepare_application_compute(command, pool, 1)
    }

    pub(crate) fn prepare_application_compute(&mut self, command: G18PreparedComputeCommand,
        pool: G18UmaPoolBinding, generation: u32) -> Result {
        self.prepare_application_compute_rebind(command, pool, pool, command.context_id, generation)
    }

    pub(crate) fn prepare_application_compute_rebind(&mut self, command: G18PreparedComputeCommand,
        pool: G18UmaPoolBinding, previous: G18UmaPoolBinding, previous_context: u32,
        generation: u32) -> Result {
        use crate::g18::fw::sksm as sksm;
        if pool.slot != previous.slot || pool.generation == u64::MAX
            || !(pool.generation == previous.generation && pool.state == previous.state
                || previous.generation.checked_add(1) == Some(pool.generation)
                    && pool.state != previous.state)
            || pool.state < KERNEL_BASE || pool.state & 0x3fff != 0 { return Err(EINVAL); }
        if generation == 0 || generation > 0x00ff_ffff
            || generation != self.compute_generation + 1 { return Err(EINVAL); }
        let before = (3 + (generation - 1) * 3) % 256;
        if self.compute_prepared || self.counters()? != [before; 3]
            || !Self::installed(self.slot())
            || !Self::scheduler_usable(self.scheduler_status()?)
            || !Self::pool_bound_for_context(self.pool_status(), previous, previous_context)
            || !(3..64).contains(&command.context_id) || command.expected_stamp != generation << 8
            || command.firmware_address < KERNEL_BASE
            || command.registers.main_count > 127 || command.registers.secondary_count != 4 {
            return Err(EINVAL);
        }
        let entry = sksm::prepare_m5_sksm_entry(sksm::M5SksmQueue {
            id: QUEUE_ID, priority: 2, current_stamp: u64::from(generation), submitted: 0,
            previous_stamp: if generation == 1 { None } else { Some(u64::from(generation - 1)) },
        }, sksm::M5SksmOperands {
            barriers: &[], first_predecessor: 0, implicit_dependency_type: 1,
            mcache: None, flag_bit1: false, flag_bit61: false, flag_bit63: false,
            payload: [command.firmware_address, GRAPH_BASE],
            qos_byte_32: 3, qos_class_40: 8, qos_word_48: 0xffff,
            // Native CL event mask uses type 2 * 64 contexts + context ID.
            event_mask: [0, 0, 1u64 << command.context_id, 0], rce_kind: 0,
            rce: [sksm::M5SksmRce { address: command.registers.main_gpu,
                                   tag: command.registers.main_count as u8 },
                  sksm::M5SksmRce { address: command.registers.secondary_gpu, tag: 4 },
                  sksm::M5SksmRce { address: 0, tag: 0 },
                  sksm::M5SksmRce { address: 0, tag: 0 }],
            auxiliary: 0x3f_ffff_ffff_ffff,
        }).map_err(|_| EINVAL)?;
        // Original 26A428 submitBuffer 8be481c..8be4848 writes this same
        // packed 21-byte form: type14, qid, count, priority, stamp, DM2.
        let kick = sksm::encode_m5_sksm_add_kicks(QUEUE_ID, 2, u64::from(generation), 1).map_err(|_| EINVAL)?;
        self._entry.with_bytes_mut(|out| {
            let bytes = &mut out[entry.offset as usize..entry.offset as usize + entry.bytes.len()];
            if generation < 256 && bytes.iter().any(|v| *v != 0) { return Err(EBUSY); }
            bytes.copy_from_slice(&entry.bytes);
            Ok(())
        })?;
        self.graph.with_bytes_mut(|out| {
            if generation == 1 && (out[CONFIG + 0x300..CONFIG + 0x500].iter().any(|v| *v != 0)
                || out[ITEMS + 24..ITEMS + 48].iter().any(|v| *v != 0)) {
                return Err(EBUSY);
            }
            out.copy_within(CONFIG + 0x200..CONFIG + 0x298, CONFIG + 0x300);
            let config = &mut out[CONFIG + 0x300..CONFIG + 0x398];
            put32(config, 0x32, command.context_id);
            put64(config, 0x36, pool.state);
            put64(config, 0x3e, pool.generation);
            put32(config, 0x46, pool.slot);
            put32(config, 0x62, 1); // native descriptor+0x190: update context
            put32(config, 0x66, 1); // retained process owner identity
            out[CONFIG + 0x400..CONFIG + 0x400 + kick.len()].copy_from_slice(&kick);
            for (index, address) in [command.firmware_address,
                GRAPH_BASE + CONFIG as u64 + 0x300,
                GRAPH_BASE + CONFIG as u64 + 0x400].into_iter().enumerate() {
                put64(out, ITEMS + ((index + before as usize) % 256) * 8, address);
            }
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        self.compute_prepared = true;
        self.compute_generation = generation;
        Ok(())
    }

    /// Publish only after process roots and submitted-counter are ready.
    /// A later failure cannot authorize resetting the shared ring or entry.
    pub(crate) fn publish_compute_job(&mut self) -> Result {
        if !self.compute_prepared || self.compute_generation == 0 { return Err(EBUSY); }
        let before = (3 + (self.compute_generation - 1) * 3) % 256;
        if self.counters()? != [before; 3] { return Err(EBUSY); }
        fence(Ordering::SeqCst);
        self.graph.write_u32(GRAPH_BASE + POINTERS as u64 + 0x40,
            (before + 3) % 256)?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// The scheduler state a submission needs once bound: the published
    /// control, allocated indices, state 4 and QID 11's membership. Unlike
    /// [`Self::scheduler_bound`] (the bind gate, checked against the first
    /// observed header) it does not pin header bytes 3..7: a recovery clears
    /// byte 5, and render's equivalent check never required them.
    pub(crate) fn scheduler_usable([pointer, header, low, high]: [u64; 4]) -> bool {
        pointer == GRAPH_BASE + CONTROL as u64 && header & 0xff != 0xff
            && (header >> 8) & 0xff != 0xff && (header >> 16) & 0xff == 4
            && low == 1u64 << QUEUE_ID && high == 0
    }

    pub(crate) fn retire_application_compute(&mut self, generation: u32) -> Result {
        if !self.compute_prepared || self.compute_generation != generation
            || self.counters()? != [(3 + generation * 3) % 256; 3] { return Err(EBUSY); }
        self.compute_prepared = false;
        Ok(())
    }

    /// The first firmware-owned precondition of
    /// [`Self::prepare_application_compute`] that does not hold, if any. A
    /// firmware recovery can reset queue state the next job requires.
    pub(crate) fn compute_unready(&mut self, pool: G18UmaPoolBinding, context: u32,
        generation: u32) -> Result<Option<&'static str>> {
        let before = (3 + generation.wrapping_sub(1) * 3) % 256;
        Ok(if self.compute_prepared { Some("prepared") }
            else if self.counters()? != [before; 3] { Some("counters") }
            else if !Self::installed(self.slot()) { Some("installed") }
            else if !Self::scheduler_usable(self.scheduler_status()?) { Some("scheduler") }
            else if !Self::pool_bound_for_context(self.pool_status(), pool, context) { Some("pool") }
            else { None })
    }

    /// Every word [`Self::compute_unready`] reads, for the kernel log.
    pub(crate) fn compute_state(&mut self) -> Result<([u32; 3], [u64; 5], [u64; 4], [u64; 9])> {
        Ok((self.counters()?, self.slot(), self.scheduler_status()?, self.pool_status()))
    }

    pub(crate) fn installed(slot: [u64; 5]) -> bool {
        slot[0] & 1 != 0 && slot[1] == ENTRY_HIGH && slot[2] == ENTRY_LOW
            && slot[3] == 4u64 << 32 && slot[4] as u32 == 2
    }
}
