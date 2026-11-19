// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8112 (G14G) with GPU firmware 27.2: the bring-up root, its queues and the command pool
//! that the compute DRM backend (`crate::j413`) publishes on.

use super::*;

/// An isolated root-layout comparison. The manager owns the calibrated G14
/// records and channels. Every new pointer names a live allocation.
pub(crate) struct G14TrialRoot {
    graph: Arc<GpuManagerG14V13_5>,
    directory: Option<fw::types::GpuArray<u8>>,
    controller: Option<fw::types::GpuArray<u8>>,
    slot_ram: KVec<G14SlotRam>,
    _channels: KVec<fw::types::GpuArray<u8>>,
    _register_mappings: KVec<mmu::KernelMapping>,
    startup_fence: Option<G14StartupFence>,
    globals: Option<fw::types::GpuArray<u8>>,
    boot_page: Option<fw::types::GpuArray<u8>>,
    status: fw::types::GpuArray<u8>,
    control: fw::types::GpuArray<u8>,
    power: fw::types::GpuArray<u8>,
    extra: fw::types::GpuArray<u8>,
}

/// Ordinary RAM only. The mapping drops before its backing. Both are retained
/// with the root through ASC shutdown, including the failed-shutdown path.
struct G14SlotRam {
    mappings: KVec<mmu::KernelMapping>,
    register_base: u64,
    ram_pages: u32,
    ram: mmu::ContiguousScratch,
    initial: KVec<u8>,
    slot: usize,
}

impl G14SlotRam {
    fn readback(slot: usize) -> bool {
        matches!(slot, 1 | 10)
    }

    fn protection(slot: usize) -> mmu::Prot {
        if Self::readback(slot) { mmu::PROT_FW_SHARED_RO } else { mmu::PROT_FW_SHARED_RW }
    }

    fn new(graph: &GpuManagerG14V13_5, slot: usize, span: usize, va: u64, register_base: u64) -> Result<Self> {
        let mut initial = KVec::from_elem(0u8, span, GFP_KERNEL)?;
        if matches!(slot, 9 | 10) {
            // Slots 9 and 10 start from a snapshot of their live register pages, which the
            // caller saves as a firmware file before loading the module.
            let name = if slot == 10 { c"apple/t8112-gpu-slot10-seed.bin" } else { c"apple/t8112-gpu-slot9-seed.bin" };
            let seed = kernel::firmware::Firmware::request_nowarn(
                name, graph.dev.as_ref().as_ref())?;
            if seed.size() != span { return Err(EINVAL); }
            initial.copy_from_slice(seed.data());
        }
        if slot == 40 {
            // G14G has one cluster. Preserve its board fuse mask in the Fender copy.
            let [mask] = graph.dyncfg.id.core_masks.as_slice() else { return Err(EINVAL); };
            initial[0x1500..0x1504].copy_from_slice(&(*mask as u32).to_le_bytes());
        }
        let ram = mmu::ContiguousScratch::zeroed(span)?;
        for (index, chunk) in initial.chunks(kernel::page::PAGE_SIZE).enumerate() {
            ram.with_cpu_page(index * kernel::page::PAGE_SIZE, |ptr| {
                // SAFETY: the allocated page is exclusively host-owned before publication.
                unsafe { core::ptr::copy_nonoverlapping(chunk.as_ptr(), ptr, chunk.len()) };
                crate::pgtable::clean_invalidate_poc(ptr, chunk.len());
            })?;
        }
        crate::pgtable::poc_barrier();
        let ram_pages = if slot == 17 {
            let pages = crate::module_parameters::t8112_rgx_ram_pages.value();
            if pages > 255 || span != 8 * mmu::UAT_PGSZ || register_base != 0x204000000 {
                return Err(EINVAL);
            }
            pages
        } else { u32::MAX };
        let mut owner = Self { mappings: KVec::new(), register_base, ram_pages, ram, initial, slot };
        for offset in (0..span).step_by(mmu::UAT_PGSZ) {
            let (physical, prot) = owner.backing(offset);
            let map = graph.uat.kernel_vm().map_io(graph.dev.as_ref().as_ref(), va + offset as u64,
                physical as usize, mmu::UAT_PGSZ, prot)?;
            if map.translate_offset(0)? != physical { return Err(EFAULT); }
            owner.mappings.push(map, GFP_KERNEL)?;
        }
        Ok(owner)
    }

    fn backing(&self, offset: usize) -> (u64, mmu::Prot) {
        if self.slot == 17 && self.ram_pages & (1 << (offset / mmu::UAT_PGSZ)) == 0 {
            (self.register_base + offset as u64, mmu::PROT_FW_MMIO_RW)
        } else {
            (self.ram.physical() + offset as u64, Self::protection(self.slot))
        }
    }

    fn report_stopped(&self, graph: &GpuManagerG14V13_5) -> Result {
        let mut changed = 0;
        for (index, chunk) in self.initial.chunks(kernel::page::PAGE_SIZE).enumerate() {
            let base = index * kernel::page::PAGE_SIZE;
            self.ram.with_cpu_page(base, |ptr| {
                crate::pgtable::clean_invalidate_poc(ptr, chunk.len());
                crate::pgtable::poc_barrier();
                for (i, bytes) in chunk.chunks_exact(4).enumerate() {
                    let before = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
                    // SAFETY: the ASC is stopped; this aligned word is within the owned page.
                    let after = unsafe { ptr.cast::<u32>().add(i).read_volatile() };
                    if after != before {
                        dev_info!(graph.dev.as_ref(), "G14G scratch slot={} offset={:#x} before={:#x} after={:#x}\n",
                                  self.slot, base + 4*i, before, after);
                        changed += 1;
                    }
                }
            })?;
        }
        dev_info!(graph.dev.as_ref(), "G14G scratch slot={} changed={} words={}\n",
                  self.slot, changed, self.initial.len()/4);
        Ok(())
    }
}

impl fw::types::GpuStruct for G14TrialRoot {
    type Raw<'a> = fw::initdata::raw::InitDataG14V27_2<'a>;
}

#[derive(Copy, Clone)]
pub(crate) enum G14DirectoryHeader {
    Zero,
    PrivatePage,
    J413 { sgx: u64, sgx_size: u64, interrupt: u64, copy_bytes: u32, live: bool, first_job: bool },
}

/// Single kernel-owned completion with no external notification destinations.
struct G14StartupFence {
    queue: fw::types::GpuArray<u8>,
    positions: fw::types::GpuArray<u8>,
    counter: fw::types::GpuArray<u8>,
    event: event::Event,
    expected_stamp: u32,
    /// Stamp value of the compute job published as the queue's second command.
    second: Option<u32>,
    _owners: KVec<fw::types::GpuArray<u8>>,
    /// Client-visible buffers of the kernel-owned compute job: mapping before backing.
    _client: Option<(mmu::KernelMapping, gem::ObjectRef, fw::types::GpuArray<u8>)>,
    /// Stream offsets that take the compute engine-state address once the directory exists.
    stats_patch: KVec<usize>,
    /// That address, for compute commands built after boot.
    compute_state: u64,
}

impl G14StartupFence {
    fn new(alloc: &mut KernelAllocators, event: event::Event, uat: &mmu::Uat, dev: &AsahiDevice) -> Result<Self> {
        let mut queue = alloc.private.array_empty::<u8>(0x2cc0)?;
        let mut positions = alloc.shared.array_empty::<u8>(0x2860)?;
        let mut pointers = alloc.private.array_empty::<u8>(0x2860)?;
        let mut scheduler = alloc.private.array_empty::<u8>(0x40)?;
        let mut timestamps = alloc.shared.array_empty::<u8>(0x18)?;
        let mut command = alloc.private.array_empty::<u8>(0x400)?;
        let mut stream = alloc.private.array_empty::<u8>(0x400)?;
        // The counter is a client VA resolved through the stream's context. The
        // kernel VM's lower half is context 0's client range (legacy GPU allocator).
        let client = crate::module_parameters::t8112_client_counter.value();
        let counter = alloc.shared.array_empty::<u8>(8)?;
        // The only context-0 client mapping the J413 graph owns is the low alias of the
        // (idle) buffer-manager control page; borrow a cleared word near its end.
        let counter_va = if client { IOVA_KERN_GPU_BUFMGR_LOW + 0x3f00 } else { counter.gpu_va().get() };
        // One command-buffer tracking slot (0x100 stride, 0xc8 maintained by firmware)
        // whose first word names its own cleared u32 retirement cursor.
        let stock = crate::module_parameters::t8112_stock_header.value();
        let mut notifier = alloc.private.array_empty::<u8>(0x100)?;
        let cursor = alloc.shared.array_empty::<u8>(0x90)?;
        if stock {
            notifier.as_mut_slice()[..8].copy_from_slice(&cursor.gpu_va().get().to_le_bytes());
            crate::pgtable::clean_poc(notifier.as_slice().as_ptr(), 0x100);
        }

        let ctx_task = |scheduler: &mut fw::types::GpuArray<u8>| {
            let task = crate::module_parameters::t8112_task_configuration.value();
            if task != 0 { scheduler.as_mut_slice()[0x26] = (task - 1) as u8; }
        };
        let timestamp_va = timestamps.gpu_va().get();
        timestamps.as_mut_slice()[8..16].copy_from_slice(&timestamp_va.to_le_bytes());
        let ctx = scheduler.as_mut_slice();
        ctx[..2].copy_from_slice(&u16::MAX.to_le_bytes());
        ctx[5] = 1;
        ctx[0x27] = 0xff;
        let queue_va = queue.gpu_va().get();
        let q = queue.as_mut_slice();
        for (offset, value) in [(0, positions.gpu_va().get()), (8, pointers.gpu_va().get()),
                                (0x10, timestamp_va), (0x18, queue_va + 0x90),
                                (0x7c, scheduler.gpu_va().get())] {
            q[offset..offset+8].copy_from_slice(&value.to_le_bytes());
        }
        // The driver-owned queue has no owning process.
        for offset in [0x2c, 0x48, 0x4c, 0x50] {
            q[offset..offset+4].copy_from_slice(&u32::MAX.to_le_bytes());
        }
        q[0x30..0x34].copy_from_slice(&4u32.to_le_bytes());
        q[0x4c..0x50].copy_from_slice(&crate::module_parameters::t8112_queue_owner.value().to_le_bytes());
        if crate::module_parameters::t8112_priority_row.value() == 16 {
            // Context priority 1 / QoS 1: physical group 2.
            q[0x30..0x34].copy_from_slice(&2u32.to_le_bytes());
            q[0x34..0x38].copy_from_slice(&2u32.to_le_bytes());
            q[0x38..0x40].copy_from_slice(&0xffff_0000_0000_0000u64.to_le_bytes());
            q[0x40..0x44].copy_from_slice(&0u32.to_le_bytes());
            q[0x44..0x48].copy_from_slice(&1u32.to_le_bytes());
        }
        if let row @ 0..=3 = crate::module_parameters::t8112_priority_row.value() {
            // Legacy J413 priority row (fw/workqueue.rs PRIORITY), as J613 extended writes row 2.
            let legacy = crate::fw::workqueue::raw::PRIORITY[row as usize];
            q[0x30..0x34].copy_from_slice(&legacy.0.to_le_bytes());
            q[0x34..0x38].copy_from_slice(&legacy.1.to_le_bytes());
            q[0x38..0x40].copy_from_slice(&legacy.2.0.to_le_bytes());
            q[0x40..0x44].copy_from_slice(&legacy.3.to_le_bytes());
            q[0x44..0x48].copy_from_slice(&legacy.4.to_le_bytes());
            q[0x48..0x4c].copy_from_slice(&legacy.5.to_le_bytes());
        }
        // Stock leaves the current lane at the all-ones sentinel and the
        // firmware assigns it on first activation; the legacy row presets it.
        match crate::module_parameters::t8112_queue_lane.value() {
            0 => {}
            1 => q[0x48..0x4c].copy_from_slice(&u32::MAX.to_le_bytes()),
            lane => q[0x48..0x4c].copy_from_slice(&(lane - 2).to_le_bytes()),
        }
        // Host scheduler context +0x26 carries the task-configuration byte.
        ctx_task(&mut scheduler);
        positions.as_mut_slice()[0x50..0x54].copy_from_slice(&crate::module_parameters::t8112_queue_capacity.value().to_le_bytes());

        // Keep the shared-event counter separate from the indexed completion stamp.
        crate::pgtable::clean_poc(q.as_ptr(), q.len());
        let stamp = u64::from(event.stamp_pointer());
        let stamp_value = event.current().next().raw();
        let stamp_value2 = event.current().next().next().raw();
        // Job order 2: the compute job is the queue's first command and the signal follows.
        let swap = crate::module_parameters::t8112_compute_job.value()
            && crate::module_parameters::t8112_job_order.value() == 2;
        let signal_value = if swap { stamp_value2 } else { stamp_value };
        let data = stream.as_mut_slice();
        data[..4].copy_from_slice(&14u32.to_le_bytes());
        for (offset, value) in [(8, 1u64), (0x10, 1), (0x18, 1),
                                (0x28, counter_va), (0x30, stamp)] {
            data[offset..offset+8].copy_from_slice(&value.to_le_bytes());
        }
        data[0x38..0x3c].copy_from_slice(&signal_value.to_le_bytes());
        data[0x4c..0x50].copy_from_slice(&crate::g15::fw::work::op::END.to_le_bytes());
        let cmd_va = command.gpu_va().get();
        let cmd = command.as_mut_slice();
        cmd[..4].copy_from_slice(&crate::module_parameters::t8112_command_type.value().to_le_bytes());
        // Hardware: the type-9 handler clears 0xc8 bytes through the packed u64 at +0xc.
        let at = crate::module_parameters::t8112_command_notifier.value() as usize;
        if at != 0 {
            if at > 0x14 { return Err(EINVAL); }
            cmd[at..at + 8].copy_from_slice(&notifier.gpu_va().get().to_le_bytes());
        }
        if stock {
            // Type-9 prefix: sequence 0 at +4, context 0 at +0x14, data master at +0x18.
            cmd[0x18..0x1c].copy_from_slice(&(G14TrialRoot::signal_engine() as u32).to_le_bytes());
        }
        if crate::module_parameters::t8112_completion_record.value() {
            // Hardware: retirement stores command +0x40 through the pointer at +0x30, i.e. a
            // 0x20-byte acknowledgement record begins at command +0x30.
            cmd[0x30..0x38].copy_from_slice(&stamp.to_le_bytes());
        }
        cmd[0x1c..0x24].copy_from_slice(&stream.gpu_va().get().to_le_bytes());
        cmd[0x24..0x28].copy_from_slice(&0x80u32.to_le_bytes());
        cmd[0x38..0x40].copy_from_slice(&stamp.to_le_bytes());
        cmd[0x40..0x44].copy_from_slice(&signal_value.to_le_bytes());
        let mut client_job = None;
        let mut stats_patch = KVec::new();
        // Optional second command on the same queue: the compute job follows the signal.
        let second = crate::module_parameters::t8112_compute_job.value()
            && crate::module_parameters::t8112_job_order.value() != 0;
        let mut command2 = alloc.private.array_empty::<u8>(0x400)?;
        let mut stream2 = alloc.private.array_empty::<u8>(0x400)?;
        let mut tracking2 = alloc.private.array_empty::<u8>(0x100)?;
        tracking2.as_mut_slice()[..8].copy_from_slice(&(cursor.gpu_va().get() + 4).to_le_bytes());
        let (command2_va, stream2_va, tracking2_va) =
            (command2.gpu_va().get(), stream2.gpu_va().get(), tracking2.gpu_va().get());
        if crate::module_parameters::t8112_compute_job.value() {
            // Kernel-owned compute job in context 0. Layout: legacy G14G V13_5 RunCompute and
            // StartCompute/Timestamp/FinalizeCompute payloads (fw/compute.rs, fw/microseq.rs,
            // queue/compute.rs) inside the 26.x wrapper as G15G 26.6.2 carries it
            // (m3_compute_layout.rs, m3_compute_sequence.rs encode_extended), at the anchors.
            const PREEMPT: usize = 0x10000; // hw/t8112.rs compute_preempt1_size
            const CLIENT_VA: u64 = IOVA_KERN_GPU_BUFMGR_LOW + 0x10_0000;
            let mut object = gem::new_kernel_object(dev, PREEMPT + 0x4000)?;
            let size = object.size();
            use kernel::io::Io;
            let map = object.vmap()?;
            for off in (0..size).step_by(8) { map.try_write64(0, off)?; }
            // CDM control stream: one Stream Terminate block (block type 2 in bits 31:29).
            let cdm = PREEMPT + 0x2000;
            map.try_write32(0x4000_0000, cdm)?;
            let mapping = object.map_at(uat.kernel_lower_vm(), CLIENT_VA, mmu::PROT_GPU_SHARED_RW, false)?;
            let queue_va = queue.gpu_va().get();
            let fw_stamp = u64::from(event.fw_stamp_pointer());
            let slot = event.slot();
            let timestamps_va = {
                let ts = alloc.shared.array_empty::<u8>(0x80)?;
                let va = ts.gpu_va().get();
                client_job = Some((object, mapping, ts));
                va
            };
            let (cmd, cmd_va, stream_va, tracking_va, stamp_value) = if second {
                (command2.as_mut_slice(), command2_va, stream2_va, tracking2_va, if swap { stamp_value } else { stamp_value2 })
            } else {
                (&mut *cmd, cmd_va, stream.gpu_va().get(), notifier.gpu_va().get(), stamp_value)
            };
            cmd.fill(0);
            let put32 = |b: &mut [u8], at: usize, v: u32| b[at..at + 4].copy_from_slice(&v.to_le_bytes());
            let put64 = |b: &mut [u8], at: usize, v: u64| b[at..at + 8].copy_from_slice(&v.to_le_bytes());
            let context = crate::module_parameters::t8112_job_context.value();
            put32(cmd, 0, 3);
            put64(cmd, 4, u64::from(second));
            put32(cmd, 0xc, context);
            put64(cmd, 0x10, tracking_va);
            // First register image (+0x18, 0x50 bytes): zero, as the 13.5 G14G path sends it.
            // Second image at +0x68 (= 13.5 JobParameters1).
            for (at, v) in [(0x00, CLIENT_VA), (0x08, CLIENT_VA + cdm as u64),
                            (0x10, CLIENT_VA + PREEMPT as u64), (0x18, CLIENT_VA + PREEMPT as u64 + 8),
                            (0x20, CLIENT_VA + PREEMPT as u64 + 16), (0x28, CLIENT_VA + PREEMPT as u64 + 24),
                            (0x30, 0x11_0000_0000), (0x38, 0x8c60), (0x58, 1)] {
                put64(cmd, 0x68 + at, v);
            }
            put64(cmd, 0x1e8, stream_va);
            put32(cmd, 0x1f0, 0x2c0);
            // Auxiliary block: 13.5 JobParameters2 preempt/CDM end in vector B,
            // EncoderParams mask in the last control word.
            put64(cmd, 0x220, CLIENT_VA);
            put64(cmd, 0x228, CLIENT_VA + cdm as u64 + 8);
            put32(cmd, 0x270, u32::MAX);
            put64(cmd, 0x298, stamp);
            put64(cmd, 0x2a0, fw_stamp);
            put32(cmd, 0x2a8, stamp_value);
            put32(cmd, 0x2b0, slot);
            // Tail: timestamp slots B/C name the start/end words.
            put64(cmd, 0x2cc, timestamps_va);
            put64(cmd, 0x2d4, timestamps_va + 8);
            put32(cmd, 0x33c, 1);
            match crate::module_parameters::t8112_tail_probe.value() {
                // Diagnostic: distinct unmapped addresses in the zero tail words.
                1 => { put64(cmd, 0x2b8, 0xffff_ffa7_0000_2000); put64(cmd, 0x2c4, 0xffff_ffa7_0000_1000);
                       put64(cmd, 0x2dc, 0xffff_ffa7_0000_3000); put64(cmd, 0x2ec, 0xffff_ffa7_0000_4000);
                       put64(cmd, 0x304, 0xffff_ffa7_0000_5000); put64(cmd, 0x314, 0xffff_ffa7_0000_6000); }
                2 => for at in [0x18usize, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0xa8, 0xb0, 0xb8,
                                0x200, 0x208, 0x210, 0x218, 0x230, 0x240, 0x250, 0x284, 0x28c,
                                0x2e4, 0x2f4, 0x2fc, 0x31c, 0x324, 0x32c, 0x334] {
                    put64(cmd, at, 0xffff_ffa7_0000_0000 + ((at as u64) << 16));
                },
                // Hardware: the handler clears 0x10 bytes through command +0x334.
                3 => put64(cmd, 0x334, timestamps_va + 0x20),
                4 => { put64(cmd, 0x334, timestamps_va + 0x20); put32(cmd, 0x33c, 0); }
                // Seed the dependency latch so the companion lookup is bypassed.
                5 => { put64(cmd, 0x334, timestamps_va + 0x20); put32(cmd, 0x340, 1); }
                // Canonical disabled form: +0x33c/+0x334/+0x324/+0x340 all zero.
                6 => put32(cmd, 0x33c, 0),
                _ => {}
            }

            let data = if second { stream2.as_mut_slice() } else { stream.as_mut_slice() };
            data.fill(0);
            put32(data, 0, 11);
            put64(data, 0x160, u64::from(second));
            put64(data, 0x04, cmd_va + 0x18);
            put64(data, 0x0c, cmd_va + 0x68);
            stats_patch.push(0x14, GFP_KERNEL)?;
            put64(data, 0x1c, queue_va);
            put32(data, 0x24, context);
            put32(data, 0x34, slot);
            put64(data, 0x3c, cmd_va + 0x1f4);
            put64(data, 0x158, cmd_va + 0x340);
            for (at, op, selected) in [(0x16c, 0x8000_0003u32, 0x2ccu64), (0x1bc, 3, 0x2d4)] {
                put32(data, at, op);
                put64(data, at + 0x04, cmd_va + 0x2c4);
                put64(data, at + 0x0c, cmd_va + 0x2cc);
                put64(data, at + 0x14, cmd_va + selected);
                put64(data, at + 0x1c, queue_va);
                put64(data, at + 0x2c, cmd_va + 0x314);
                put64(data, at + 0x34, cmd_va + 0x264);
            }
            match crate::module_parameters::t8112_start_probe.value() {
                // Diagnostics: unmapped addresses in the start record's private operands.
                1 => { stats_patch.clear(); put64(data, 0x14, 0xffff_ffa7_0014_0000); }
                2 => put64(data, 0x1c, 0xffff_ffa7_001c_0000),
                // Legacy StartCompute values the table leaves as descriptor scalars:
                // unk_28 = 1, event_generation = queue id, uuid.
                4 => put32(data, 0x28, 1),
                5 => { put32(data, 0x28, 1); put32(data, 0x2c, 1); put32(data, 0x30, 1); }
                3 => { put64(data, 0x04, 0xffff_ffa7_0004_0000); put64(data, 0x0c, 0xffff_ffa7_000c_0000);
                       put64(data, 0x3c, 0xffff_ffa7_003c_0000); put64(data, 0x158, 0xffff_ffa7_0158_0000); }
                _ => {}
            }
            put32(data, 0x1b8, 0x0080_0000);
            let finish = 0x208;
            put32(data, finish, 12);
            stats_patch.push(finish + 4, GFP_KERNEL)?;
            put64(data, finish + 0x0c, queue_va);
            put32(data, finish + 0x14, context);
            put64(data, finish + 0x18, cmd_va + 0x1f4);
            put64(data, finish + 0x2c, fw_stamp);
            put32(data, finish + 0x34, stamp_value);
            put32(data, finish + 0x5c, (-(finish as i32)) as u32);
            put64(data, finish + 0x71, cmd_va + 0x340);
            put64(data, finish + 0x79, cmd_va + 0x304);
            put32(data, finish + 0x84, 0x4000_0002);
        }
        crate::pgtable::clean_poc(stream.as_slice().as_ptr(), stream.as_slice().len());
        crate::pgtable::clean_poc(cmd.as_ptr(), cmd.len());
        crate::pgtable::poc_barrier();
        pointers.as_mut_slice()[..8].copy_from_slice(&command.gpu_va().get().to_le_bytes());
        if swap {
            pointers.as_mut_slice()[..8].copy_from_slice(&command2_va.to_le_bytes());
        }
        if second {
            for owner in [&command2, &stream2, &tracking2] {
                crate::pgtable::clean_poc(owner.as_slice().as_ptr(), owner.as_slice().len());
            }
            crate::pgtable::poc_barrier();
            let next = if swap { command.gpu_va().get() } else { command2_va };
            pointers.as_mut_slice()[8..16].copy_from_slice(&next.to_le_bytes());
        }

        // These allocations are private to this fresh graph. Walk its owned RAM
        // tables before publishing anything to the ASC, including both edge pages
        // of each suballocation. G14G's upper root takes a 39-bit input.
        let root = uat.kernel_window_root();
        let check = |address: u64, len: usize, expected: mmu::Prot| -> Result {
            let end = address.checked_add(len as u64 - 1).ok_or(EOVERFLOW)?;
            let mut page = address & !(mmu::UAT_PGMSK as u64);
            while page <= end {
                let entry = crate::g15::uat::read_leaf(root, page & ((1u64 << 39) - 1))?
                    .ok_or(EFAULT)?;
                if entry & 3 != 3 || mmu::Prot::from_pte(entry).leaf_bits() != expected.leaf_bits() {
                    return Err(EFAULT);
                }
                page = page.checked_add(mmu::UAT_PGSZ as u64).ok_or(EOVERFLOW)?;
            }
            Ok(())
        };
        for owner in [&queue, &pointers, &scheduler, &stream, &command] {
            check(owner.gpu_va().get(), owner.as_slice().len(), mmu::PROT_FW_PRIV_RW)?;
        }
        for owner in [&positions, &timestamps] {
            check(owner.gpu_va().get(), owner.as_slice().len(), mmu::PROT_FW_SHARED_RW)?;
        }
        if client {
            // The context-0 client tree must already translate the counter page.
            let va = counter_va;
            let leaf = crate::g15::uat::read_leaf(uat.kernel_lower_root(), va)?.ok_or(ENOENT)?;
            if leaf & 3 != 3 { return Err(ENXIO); }
        } else {
            check(counter.gpu_va().get(), 8, mmu::PROT_FW_SHARED_RW)?;
        }
        check(stamp, 4, mmu::PROT_FW_SHARED_RW)?;

        let mut owners = KVec::new();
        for owner in [&timestamps, &pointers, &scheduler] {
            crate::pgtable::clean_poc(owner.as_slice().as_ptr(), owner.as_slice().len());
        }
        for owner in [timestamps, pointers, scheduler, command, stream, notifier, cursor, command2, stream2, tracking2] {
            owners.push(owner, GFP_KERNEL)?;
        }
        let _client = client_job.map(|(object, mapping, ts)| (mapping, object, ts));
        Ok(Self { queue, positions, counter, event, expected_stamp: stamp_value, second: second.then_some(stamp_value2), _owners: owners, _client, stats_patch, compute_state: 0 })
    }
}

/// Command slots for compute work published after boot (the DRM device). They are allocated
/// with the trial queue, before the firmware window's tables are mirrored for the ASC, so every
/// slot lies in a table the firmware already translates. Slots are reused in order.
pub(crate) const J413_POOL: usize = 48;

pub(crate) struct J413Pool {
    commands: fw::types::GpuArray<u8>,
    streams: fw::types::GpuArray<u8>,
    tracking: fw::types::GpuArray<u8>,
    cursors: fw::types::GpuArray<u8>,
    stamps: fw::types::GpuArray<u8>,
}

/// One compute command from a client, as the 26.x front end hands it over.
pub(crate) struct J413Compute {
    pub(crate) context: u32,
    pub(crate) sequence: u64,
    pub(crate) cdm_base: u64,
    pub(crate) cdm_end: u64,
    pub(crate) usc_base: u64,
    /// Client VA of a 0x10020-byte preemption buffer in the same context (13.5 layout:
    /// buffer 1 at +0, buffers 2 to 5 at +0x10000 + 8n).
    pub(crate) preempt: u64,
}

/// Firmware-visible facts of the trial queue that every runtime command names.
#[derive(Copy, Clone)]
pub(crate) struct J413Queue {
    pub(crate) queue: u64,
    pub(crate) slot: u32,
    pub(crate) stamp: u64,
    pub(crate) fw_stamp: u64,
    pub(crate) compute_state: u64,
}

impl J413Pool {
    pub(crate) fn new(alloc: &mut KernelAllocators) -> Result<Self> {
        let commands = alloc.private.array_empty::<u8>(J413_POOL * 0x400)?;
        let streams = alloc.private.array_empty::<u8>(J413_POOL * 0x400)?;
        let mut tracking = alloc.private.array_empty::<u8>(J413_POOL * 0x100)?;
        // One 64-byte line per retirement cursor, so no firmware write can reach a neighbour.
        let cursors = alloc.shared.array_empty::<u8>(J413_POOL * 0x40)?;
        let stamps = alloc.shared.array_empty::<u8>(J413_POOL * 0x80)?;
        // Each tracking slot's first word names its own cleared retirement cursor.
        let base = cursors.gpu_va().get();
        for (i, chunk) in tracking.as_mut_slice().chunks_mut(0x100).enumerate() {
            chunk[..8].copy_from_slice(&(base + 0x40 * i as u64).to_le_bytes());
        }
        crate::pgtable::clean_poc(tracking.as_slice().as_ptr(), tracking.as_slice().len());
        Ok(Self { commands, streams, tracking, cursors, stamps })
    }

    /// Builds slot `i` as the boot compute job is built (26B123 layout with the 13.5 G14G
    /// JobParameters payload) and returns the command's firmware VA.
    pub(crate) fn encode(&mut self, i: usize, q: &J413Queue, stamp_value: u32, job: &J413Compute) -> Result<u64> {
        if i >= J413_POOL { return Err(EINVAL); }
        let cmd_va = self.commands.gpu_va().get() + (i * 0x400) as u64;
        let stream_va = self.streams.gpu_va().get() + (i * 0x400) as u64;
        let tracking_va = self.tracking.gpu_va().get() + (i * 0x100) as u64;
        let timestamps_va = self.stamps.gpu_va().get() + (i * 0x80) as u64;
        // The firmware maintains 0xc8 bytes of the tracking slot; start each use cleared.
        let tracking = &mut self.tracking.as_mut_slice()[i * 0x100..(i + 1) * 0x100];
        tracking[8..].fill(0);
        crate::pgtable::clean_poc(tracking.as_ptr(), tracking.len());

        self.stamps.as_mut_slice()[i * 0x80..(i + 1) * 0x80].fill(0);
        crate::pgtable::clean_poc(self.stamps.as_slice()[i * 0x80..].as_ptr(), 0x80);
        let put32 = |b: &mut [u8], at: usize, v: u32| b[at..at + 4].copy_from_slice(&v.to_le_bytes());
        let put64 = |b: &mut [u8], at: usize, v: u64| b[at..at + 8].copy_from_slice(&v.to_le_bytes());
        let cmd = &mut self.commands.as_mut_slice()[i * 0x400..(i + 1) * 0x400];
        cmd.fill(0);
        put32(cmd, 0, 3);
        put64(cmd, 4, job.sequence);
        put32(cmd, 0xc, job.context);
        put64(cmd, 0x10, tracking_va);
        // 13.5 JobParameters1 at +0x68 (queue/compute.rs values; preempt offsets per 13.5).
        let p2 = job.preempt + 0x10000;
        for (at, v) in [(0x00, job.preempt), (0x08, job.cdm_base), (0x10, p2), (0x18, p2 + 8),
                        (0x20, p2 + 16), (0x28, p2 + 24), (0x30, job.usc_base), (0x38, 0x8c60), (0x58, 1)] {
            put64(cmd, 0x68 + at, v);
        }
        put64(cmd, 0x1e8, stream_va);
        put32(cmd, 0x1f0, 0x2c0);
        // 13.5 JobParameters2: preemption buffer and CDM end; EncoderParams mask.
        put64(cmd, 0x220, job.preempt);
        put64(cmd, 0x228, job.cdm_end);
        put32(cmd, 0x270, u32::MAX);
        put64(cmd, 0x298, q.stamp);
        put64(cmd, 0x2a0, q.fw_stamp);
        put32(cmd, 0x2a8, stamp_value);
        put32(cmd, 0x2b0, q.slot);
        put64(cmd, 0x2cc, timestamps_va);
        put64(cmd, 0x2d4, timestamps_va + 8);
        // The baseline's command-local latch object (enabled form, seed 0).
        put32(cmd, 0x33c, 1);
        put64(cmd, 0x334, timestamps_va + 0x20);
        let data = &mut self.streams.as_mut_slice()[i * 0x400..(i + 1) * 0x400];
        data.fill(0);
        put32(data, 0, 11);
        put64(data, 0x160, job.sequence);
        put64(data, 0x04, cmd_va + 0x18);
        put64(data, 0x0c, cmd_va + 0x68);
        put64(data, 0x14, q.compute_state);
        put64(data, 0x1c, q.queue);
        put32(data, 0x24, job.context);
        put32(data, 0x34, q.slot);
        put64(data, 0x3c, cmd_va + 0x1f4);
        put64(data, 0x158, cmd_va + 0x340);
        for (at, op, selected) in [(0x16c, 0x8000_0003u32, 0x2ccu64), (0x1bc, 3, 0x2d4)] {
            put32(data, at, op);
            put64(data, at + 0x04, cmd_va + 0x2c4);
            put64(data, at + 0x0c, cmd_va + 0x2cc);
            put64(data, at + 0x14, cmd_va + selected);
            put64(data, at + 0x1c, q.queue);
            put64(data, at + 0x2c, cmd_va + 0x314);
            put64(data, at + 0x34, cmd_va + 0x264);
        }
        put32(data, 0x1b8, 0x0080_0000);
        let finish = 0x208;
        put32(data, finish, 12);
        put64(data, finish + 4, q.compute_state);
        put64(data, finish + 0x0c, q.queue);
        put32(data, finish + 0x14, job.context);
        put64(data, finish + 0x18, cmd_va + 0x1f4);
        put64(data, finish + 0x2c, q.fw_stamp);
        put32(data, finish + 0x34, stamp_value);
        put32(data, finish + 0x5c, (-(finish as i32)) as u32);
        put64(data, finish + 0x71, cmd_va + 0x340);
        put64(data, finish + 0x79, cmd_va + 0x304);
        put32(data, finish + 0x84, 0x4000_0002);
        crate::pgtable::clean_poc(data.as_ptr(), data.len());
        crate::pgtable::clean_poc(self.commands.as_slice()[i * 0x400..].as_ptr(), 0x400);
        crate::pgtable::poc_barrier();
        Ok(cmd_va)
    }
}

impl J413Pool {
    /// Retirement: advance slot `i`'s cursor to the expected value the firmware recorded
    /// at tracking +0xc, so its retirement scan unlinks the slot from the queue's list.
    pub(crate) fn retire(&self, i: usize) -> Result<u32> {
        if i >= J413_POOL { return Err(EINVAL); }
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[i * 0x100..].as_ptr(), 0x100);
        crate::pgtable::poc_barrier();
        let expected = self.tracking.shared_index(i * 0x100 + 0xc)?.load(Ordering::Acquire);
        self.cursors.shared_index(i * 0x40)?.store(expected, Ordering::Release);
        crate::pgtable::clean_poc(self.cursors.as_slice()[i * 0x40..].as_ptr(), 0x40);
        crate::pgtable::poc_barrier();
        Ok(expected)
    }

    /// Whether the firmware still holds slot `i` in a queue's tracking list (+0x94).
    pub(crate) fn registered(&self, i: usize) -> Result<bool> {
        if i >= J413_POOL { return Err(EINVAL); }
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[i * 0x100..].as_ptr(), 0x100);
        crate::pgtable::poc_barrier();
        Ok(self.tracking.shared_index(i * 0x100 + 0x94)?.load(Ordering::Acquire) != 0)
    }

    /// Logs slot `i`'s tracking slot, cursor, command head and stream start.
    pub(crate) fn dump(&self, dev: &kernel::device::Device, i: usize) {
        let w = |a: &fw::types::GpuArray<u8>, at: usize| a.shared_index(at).map(|x| x.load(Ordering::Acquire)).unwrap_or(u32::MAX);
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[i * 0x100..].as_ptr(), 0x100);
        crate::pgtable::clean_invalidate_poc(self.cursors.as_slice()[i * 0x40..].as_ptr(), 0x40);
        crate::pgtable::poc_barrier();
        let mut t = [0u32; 0x32];
        for (k, x) in t.iter_mut().enumerate() { *x = w(&self.tracking, i * 0x100 + 4 * k); }
        dev_info!(dev, "J413 slot {} cursor={:#x} tracking={:x?}\n", i, w(&self.cursors, i * 0x40), t);
        let mut c = [0u32; 8];
        for (k, x) in c.iter_mut().enumerate() { *x = w(&self.commands, i * 0x400 + 4 * k); }
        let mut st = [0u32; 8];
        for (k, x) in st.iter_mut().enumerate() { *x = w(&self.streams, i * 0x400 + 4 * k); }
        dev_info!(dev, "J413 slot {} command={:x?} stream={:x?} stamp+0x2a8={:#x}\n", i, c, st, w(&self.commands, i * 0x400 + 0x2a8));
    }
}

impl G14TrialRoot {
    /// Logs the queue's work record for `ordinal`.
    pub(crate) fn j413_record(&self, ordinal: usize) {
        if let Some(job) = &self.startup_fence {
            let at = 0x90 + (ordinal & 0x7f) * 0x58;
            let q = job.queue.as_slice();
            crate::pgtable::clean_invalidate_poc(q[at..].as_ptr(), 0x58);
            crate::pgtable::poc_barrier();
            let mut r = [0u32; 0x16];
            for (k, x) in r.iter_mut().enumerate() { *x = u32::from_le_bytes([q[at+4*k], q[at+4*k+1], q[at+4*k+2], q[at+4*k+3]]); }
            dev_info!(self.graph.dev.as_ref(), "J413 work record ordinal {} +{:#x}: {:x?}\n", ordinal & 0x7f, at, r);
        }
    }

    /// The trial queue's identity for runtime commands, and the stamp value it last reached.
    pub(crate) fn j413_queue(&self) -> Result<(J413Queue, event::EventValue)> {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        if job.compute_state == 0 { return Err(EINVAL); }
        Ok((J413Queue {
            queue: job.queue.gpu_va().get(),
            slot: job.event.slot(),
            stamp: u64::from(job.event.stamp_pointer()),
            fw_stamp: u64::from(job.event.fw_stamp_pointer()),
            compute_state: job.compute_state,
        }, job.event.current()))
    }

    /// The queue's capacity (position block +0x50).
    pub(crate) fn j413_capacity(&self) -> Result<u32> {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        Ok(job.positions.shared_index(0x50)?.load(Ordering::Acquire))
    }

    /// Current queue head, the firmware's retired position and the stamp value.
    pub(crate) fn j413_progress(&self) -> Result<(u32, u32, event::EventValue)> {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        let positions = job.positions.as_slice();
        crate::pgtable::clean_invalidate_poc(positions.as_ptr(), 0x60);
        crate::pgtable::poc_barrier();
        Ok((job.positions.shared_index(0x40)?.load(Ordering::Acquire),
            job.positions.shared_index(0)?.load(Ordering::Acquire), job.event.current()))
    }

    /// Publishes `command` as the queue's next command: pointer, queue head, then the compute
    /// submission entry and its producer, as m3_config::submit_queue does on the same ring.
    pub(crate) fn j413_publish(&self, command: u64) -> Result<u32> {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        let pointers = &job._owners[1];
        let head = job.positions.shared_index(0x40)?;
        let old = head.load(Ordering::Acquire);
        let capacity = job.positions.shared_index(0x50)?.load(Ordering::Acquire);
        if capacity == 0 || (old % capacity) as usize * 8 + 8 > pointers.as_slice().len() { return Err(EINVAL); }
        let at = (old % capacity) as usize * 8;
        pointers.shared_index(at)?.store(command as u32, Ordering::Relaxed);
        pointers.shared_index(at + 4)?.store((command >> 32) as u32, Ordering::Relaxed);
        crate::pgtable::clean_poc(pointers.as_slice()[at..].as_ptr(), 8);
        crate::pgtable::poc_barrier();
        let new = (old + 1) % capacity;
        head.store(new, Ordering::Release);
        crate::pgtable::clean_poc(job.positions.as_slice().as_ptr(), 0x60);
        crate::pgtable::poc_barrier();
        let state = &self._channels[2 * Self::signal_ring()];
        let ring = &self._channels[2 * Self::signal_ring() + 1];
        let w = state.shared_index(0x20)?.load(Ordering::Acquire);
        if w >= crate::m3::m3_channel_format::SLOTS { return Err(EIO); }
        let slot = u8::try_from(job.event.slot()).map_err(|_| EINVAL)?;
        let entry = crate::m3::submission_entry(Self::signal_engine() as u32, job.queue.gpu_va().get(),
            u16::try_from(new).map_err(|_| EINVAL)?, slot, false).map_err(|_| EINVAL)?;
        let base = w as usize * entry.len();
        for (k, word) in entry.chunks(4).enumerate() {
            ring.shared_index(base + 4 * k)?.store(u32::from_le_bytes([word[0], word[1], word[2], word[3]]), Ordering::Relaxed);
        }
        crate::pgtable::clean_poc(ring.as_slice()[base..].as_ptr(), entry.len());
        crate::pgtable::poc_barrier();
        state.shared_index(0x20)?.store((w + 1) % crate::m3::m3_channel_format::SLOTS, Ordering::Release);
        crate::pgtable::clean_poc(state.as_slice().as_ptr(), 0x30);
        crate::pgtable::poc_barrier();
        Ok(new)
    }

    /// Logs the compute submission ring's index words, its last entries and the position block.
    pub(crate) fn j413_rings(&self) -> Result {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        let state = &self._channels[2 * Self::signal_ring()];
        let ring = &self._channels[2 * Self::signal_ring() + 1];
        let w = |a: &fw::types::GpuArray<u8>, at: usize| a.shared_index(at).map(|x| x.load(Ordering::Acquire)).unwrap_or(u32::MAX);
        let idx: [u32; 12] = core::array::from_fn(|i| w(state, 4 * i));
        let pos: [u32; 24] = core::array::from_fn(|i| w(&job.positions, 4 * i));
        dev_info!(self.graph.dev.as_ref(), "J413 ring index={:x?} positions={:x?}\n", idx, pos);
        for e in [254usize, 255, 0, 1] {
            let words: [u32; 6] = core::array::from_fn(|i| w(ring, e * 0x18 + 4 * i));
            dev_info!(self.graph.dev.as_ref(), "J413 ring entry {} {:x?}\n", e, words);
        }
        Ok(())
    }

    /// Logs the last `count` firmware trace records (already consumed or not).
    pub(crate) fn j413_trace_tail(&self, count: u32) -> Result {
        let state = &self._channels[2 * 15];
        let ring = &self._channels[2 * 15 + 1];
        let write = state.shared_index(0x20)?.load(Ordering::Acquire);
        if write >= 512 { return Err(EIO); }
        for k in (1..=count.min(511)).rev() {
            let at = (write + 512 - k) % 512;
            let mut words = [0u32; 12];
            for (i, word) in words.iter_mut().enumerate() {
                *word = ring.shared_index(at as usize * 0x48 + 4 * i)?.load(Ordering::Acquire);
            }
            dev_info!(self.graph.dev.as_ref(), "J413 trace[{}] {:x?}\n", at, words);
        }
        Ok(())
    }

    /// Diagnostic: one submission entry naming the current queue head (no new command).
    pub(crate) fn j413_publish_entry(&self) -> Result {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        let head = job.positions.shared_index(0x40)?.load(Ordering::Acquire);
        let state = &self._channels[2 * Self::signal_ring()];
        let ring = &self._channels[2 * Self::signal_ring() + 1];
        let w = state.shared_index(0x20)?.load(Ordering::Acquire);
        if w >= crate::m3::m3_channel_format::SLOTS { return Err(EIO); }
        let slot = u8::try_from(job.event.slot()).map_err(|_| EINVAL)?;
        let entry = crate::m3::submission_entry(Self::signal_engine() as u32, job.queue.gpu_va().get(),
            u16::try_from(head).map_err(|_| EINVAL)?, slot, false).map_err(|_| EINVAL)?;
        let base = w as usize * entry.len();
        for (k, word) in entry.chunks(4).enumerate() {
            ring.shared_index(base + 4 * k)?.store(u32::from_le_bytes([word[0], word[1], word[2], word[3]]), Ordering::Relaxed);
        }
        crate::pgtable::clean_poc(ring.as_slice()[base..].as_ptr(), entry.len());
        crate::pgtable::poc_barrier();
        state.shared_index(0x20)?.store((w + 1) % crate::m3::m3_channel_format::SLOTS, Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok(())
    }

    /// Kick message for the trial queue's submission ring.
    pub(crate) fn j413_kick() -> u64 {
        (0x83u64 << 48) | (4 * Self::signal_group() + Self::signal_engine()) as u64
    }

    pub(crate) fn publish_startup_fence(&self, operating_point: u32) -> Result {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        // This trial ring starts empty. Generation one holds power and generation
        // two permits idle again. Verify the hold before publishing either index.
        let index = operating_point & 0xf;
        if operating_point & 0x10 != 0 || index == 0 || index > self.graph.dyncfg.pwr.perf_max_pstate
            || self.idle_progress()? != [Self::control_base() + 1; 3]
            || self.idle_acknowledgement()? != self.globals.as_ref().ok_or(EINVAL)?.shared_index(0xe40)?.load(Ordering::Acquire) {
            return Err(EAGAIN);
        }
        let service = self.command_service()?;
        if crate::module_parameters::t8112_command_service.value() && service.1 != 0
            || crate::module_parameters::t8112_service_stage.value() && service.0 != 2 {
            return Err(EAGAIN);
        }
        let producer = self._channels[2 * Self::signal_ring()].shared_index(0x20)?;
        let head = job.positions.shared_index(0x40)?;
        if self.status.shared_index(0x14)?.load(Ordering::Acquire) != 1 ||
            producer.load(Ordering::Acquire) != Self::ring_index(0) || head.load(Ordering::Acquire) != 0 {
            return Err(EAGAIN);
        }
        crate::pgtable::poc_barrier();
        head.store(1, Ordering::Release);
        crate::pgtable::poc_barrier();
        producer.store(Self::ring_index(1), Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok(())
    }

    /// Publish the queue's second command (the compute job) after the signal has retired.
    pub(crate) fn publish_second(&self) -> Result {
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        if job.second.is_none() { return Err(EINVAL); }
        let producer = self._channels[2 * Self::signal_ring()].shared_index(0x20)?;
        let head = job.positions.shared_index(0x40)?;
        if producer.load(Ordering::Acquire) != Self::ring_index(1) || head.load(Ordering::Acquire) != 1 { return Err(EAGAIN); }
        crate::pgtable::poc_barrier();
        head.store(2, Ordering::Release);
        crate::pgtable::poc_barrier();
        producer.store(Self::ring_index(2), Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok(())
    }

    pub(crate) fn second_complete(&self) -> Result<bool> {
        self.drain_statistics()?;
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        Ok(job.positions.shared_index(0)?.load(Ordering::Acquire) == 2
            && Some(job.event.current().raw()) == job.second)
    }

    pub(crate) fn startup_fence_complete(&self) -> Result<bool> {
        self.drain_statistics()?;
        let job = self.startup_fence.as_ref().ok_or(EINVAL)?;
        Ok(self._channels[2 * Self::signal_ring()].shared_index(0)?.load(Ordering::Acquire) == Self::ring_index(1)
            && job.positions.shared_index(0x30)?.load(Ordering::Acquire) == 1
            && job.positions.shared_index(0)?.load(Ordering::Acquire) == 1
            && (crate::module_parameters::t8112_client_counter.value()
                || (crate::module_parameters::t8112_compute_job.value()
                    && (job.second.is_none() || crate::module_parameters::t8112_job_order.value() == 2))
                || job.counter.shared_index(0)?.load(Ordering::Acquire) == 1)
            && job.event.current().raw() == job.expected_stamp)
    }

    /// Consume everything the firmware has published on the statistics ring, as the
    /// J613 drain does, so its producer never waits for space. Records are not kept.
    pub(crate) fn drain_statistics(&self) -> Result {
        if !crate::module_parameters::t8112_drain_statistics.value() || self._channels.len() < 34 { return Ok(()); }
        let state = &self._channels[2 * 16];
        let write = state.shared_index(0x20)?.load(Ordering::Acquire);
        let read = state.shared_index(0)?;
        if write < 256 && read.load(Ordering::Acquire) != write {
            read.store(write, Ordering::Release);
            crate::pgtable::poc_barrier();
        }
        // Firmware-to-host event ring (256 records of 0x38 bytes): the firmware waits for space when it is
        // full, and host consumer advancement is the acknowledgement. Completion is followed
        // through the queues' position blocks, so the records are only counted and the
        // first few logged.
        static EVENTS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
        let state = &self._channels[2 * 13];
        let ring = &self._channels[2 * 13 + 1];
        let write = state.shared_index(0x20)?.load(Ordering::Acquire);
        let read = state.shared_index(0)?;
        let mut at = read.load(Ordering::Acquire);
        if write < 256 && at < 256 && at != write {
            while at != write {
                if EVENTS.fetch_add(1, Ordering::Relaxed) < 4 {
                    let words: [u32; 6] = core::array::from_fn(|i|
                        ring.shared_index(at as usize * 0x38 + 4 * i).map(|w| w.load(Ordering::Acquire)).unwrap_or(u32::MAX));
                    dev_info!(self.graph.dev.as_ref(), "G14G event[{}] {:x?}\n", at, words);
                }
                at = (at + 1) % 256;
            }
            read.store(at, Ordering::Release);
            crate::pgtable::poc_barrier();
        }
        // Firmware log rings (one 256-record ring per log thread, state blocks 0x30 apart):
        // consumed the same way so they cannot fill on a long-running device.
        let state = &self._channels[2 * 14];
        for thread in 0..crate::m3::m3_init_26::LOG_THREADS {
            let (Ok(write), Ok(read)) = (state.shared_index(thread * 0x30 + 0x20), state.shared_index(thread * 0x30))
                else { break };
            let write = write.load(Ordering::Acquire);
            if write < 256 && read.load(Ordering::Acquire) != write {
                read.store(write, Ordering::Release);
                crate::pgtable::poc_barrier();
            }
        }
        // Firmware trace ring. Hardware uses the J613 geometry (512 records of 0x48 bytes),
        // not 256 x 0x90. Consume in order; print the first records.
        static SHOWN: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
        let state = &self._channels[2 * 15];
        let ring = &self._channels[2 * 15 + 1];
        let write = state.shared_index(0x20)?.load(Ordering::Acquire);
        let read = state.shared_index(0)?;
        let mut at = read.load(Ordering::Acquire);
        if write < 512 && at < 512 {
            while at != write {
                if SHOWN.fetch_add(1, Ordering::Relaxed) < crate::module_parameters::t8112_trace_show.value() {
                    let mut words = [0u32; 12];
                    for (i, word) in words.iter_mut().enumerate() {
                        *word = ring.shared_index(at as usize * 0x48 + 4 * i)?.load(Ordering::Acquire);
                    }
                    dev_info!(self.graph.dev.as_ref(), "G14G trace[{}] {:x?}\n", at, words);
                }
                at = (at + 1) % 512;
            }
            read.store(at, Ordering::Release);
            crate::pgtable::poc_barrier();
        }
        Ok(())
    }

    /// Shared status +0x10: non-zero while the firmware reports itself powered/running.
    pub(crate) fn firmware_running(&self) -> Result<u32> {
        Ok(self.status.shared_index(0x10)?.load(Ordering::Acquire))
    }

    /// Command-service state: (powered stage, pending installation request).
    /// Engine entries may be published only at (2, 0).
    pub(crate) fn command_service(&self) -> Result<(u32, u32)> {
        Ok((self.control.shared_index(0x4c00)?.load(Ordering::Acquire),
            self.control.shared_index(0x4c24)?.load(Ordering::Acquire)))
    }

    pub(crate) fn idle_acknowledgement(&self) -> Result<u32> {
        Ok(self.control.shared_index(0xe838)?.load(Ordering::Acquire))
    }

    pub(crate) fn idle_progress(&self) -> Result<[u32; 3]> {
        self.drain_statistics()?;
        let ring = &self._channels[2 * crate::m3::DEVICE_CONTROL];
        let read = |offset| ring.shared_index(offset).map(|v| v.load(Ordering::Acquire));
        Ok([read(0)?, read(0x10)?, read(0x20)?])
    }

    /// Data master of the kernel-owned signal at priority zero: tiler, fragment or compute.
    pub(crate) fn signal_engine() -> usize {
        crate::module_parameters::t8112_command_engine.value().min(2) as usize
    }

    /// Priority group of the signal. The stock layout leaves the group to the configured priority; the
    /// legacy J413 and J613 queues submit in the group their priority row names.
    pub(crate) fn signal_group() -> usize {
        match crate::module_parameters::t8112_priority_row.value() { p @ 0..=3 => p as usize, 16 => 2, _ => 0 }
    }

    /// First entry of the trial ring (diagnostic; 0 is the stock empty ring).
    pub(crate) fn ring_start() -> u32 {
        crate::module_parameters::t8112_ring_start.value() % crate::m3::m3_channel_format::SLOTS
    }

    /// Ring index `n` publications after the start.
    pub(crate) fn ring_index(n: u32) -> u32 {
        (Self::ring_start() + n) % crate::m3::m3_channel_format::SLOTS
    }

    /// Index of the signal's submission descriptor among the twelve.
    pub(crate) fn signal_ring() -> usize {
        3 * Self::signal_group() + Self::signal_engine()
    }

    /// Control records that precede the two idle-policy records on the trial ring:
    /// the J613 extended opening record, then its six engine-class registrations.
    pub(crate) fn control_base() -> u32 {
        match crate::module_parameters::t8112_engine_classes.value() {
            0 => 0,
            1 => 1,
            _ => 1 + crate::m3::ENGINE_CLASSES,
        }
    }

    /// Publish the next pre-encoded engine-class registration. The opening record
    /// and every earlier registration must already have retired.
    pub(crate) fn publish_engine_class(&self, ordinal: u32) -> Result<u32> {
        if ordinal >= crate::m3::ENGINE_CLASSES || Self::control_base() != 1 + crate::m3::ENGINE_CLASSES {
            return Err(EINVAL);
        }
        let producer = self._channels[2 * crate::m3::DEVICE_CONTROL].shared_index(0x20)?;
        if self.status.shared_index(0x14)?.load(Ordering::Acquire) != 1 ||
            crate::m3::idle_cursor(self.idle_progress()?).map_err(|_| EBUSY)? != 1 + ordinal {
            return Err(EAGAIN);
        }
        crate::pgtable::poc_barrier();
        producer.store(ordinal + 2, Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok(ordinal + 2)
    }

    /// Publish one of two pre-encoded policy records on a fresh idle trial ring.
    /// Returns the ring cursor and the counted generation the firmware acknowledges.
    /// As in the J613 extended control path, the generation follows the larger of
    /// the host's last publication and the firmware's consumed count.
    pub(crate) fn publish_idle_policy(&self, allow_off: bool) -> Result<(u32, u32)> {
        let cursor = Self::control_base() + u32::from(allow_off);
        let host = self.globals.as_ref().ok_or(EINVAL)?.shared_index(0xe40)?;
        let firmware = self.idle_acknowledgement()?;
        let producer = self._channels[2 * crate::m3::DEVICE_CONTROL].shared_index(0x20)?;
        if self.status.shared_index(0x14)?.load(Ordering::Acquire) != 1 ||
            crate::m3::idle_cursor(self.idle_progress()?).map_err(|_| EBUSY)? != cursor {
            return Err(EAGAIN);
        }
        let action = crate::m3::Action::Idle { enabled: allow_off };
        let generation = action.generation(host.load(Ordering::Acquire), firmware).map_err(|_| EOVERFLOW)?;
        host.store(generation, Ordering::Release);
        crate::pgtable::poc_barrier();
        producer.store(cursor + 1, Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok((cursor + 1, generation))
    }

    /// After the ASC has stopped: receive-ring cursors, the first records of each
    /// ring and the firmware's own log text. Layout shared with the J613 drain.
    pub(crate) fn report_stopped_receive(&self) {
        use crate::m3::m3_init_26 as extended;
        let dev = self.graph.dev.as_ref();
        if self._channels.len() < 34 { return; }
        // The payload owner follows other retained owners: find it by its directory address.
        let payload = self.directory.as_ref().and_then(|dir| dir.as_slice().get(0x200..0x208))
            .and_then(|b| <[u8; 8]>::try_from(b).ok()).map(u64::from_le_bytes)
            .and_then(|va| self._channels.iter().find(|a| a.gpu_va().get() == va));
        let word = |b: &[u8], at: usize| u32::from_le_bytes([b[at], b[at+1], b[at+2], b[at+3]]);
        for (name, channel, capacity, threads) in [("event", 13usize, 256usize, 1usize),
            ("log", 14, 256, extended::LOG_THREADS), ("trace", 15, 512, 1), ("stats", 16, 256, 1)] {
            let state = self._channels[2 * channel].as_slice();
            let ring = self._channels[2 * channel + 1].as_slice();
            for thread in 0..threads {
                let write = word(state, thread * 0x30 + 0x20) as usize;
                if write == 0 || write >= capacity { continue; }
                dev_info!(dev, "G14G receive {}[{}] read={} target={} write={}\n", name, thread,
                          word(state, thread * 0x30), word(state, thread * 0x30 + 0x10), write);
                for index in 0..write.min(if channel == 14 { 24 } else { 6 }) {
                    let at = (thread * capacity + index) * extended::RX_BYTES;
                    let Some(record) = ring.get(at..at + extended::RX_BYTES) else { continue; };
                    if channel == 14 {
                        let slot = word(record, 8);
                        dev_info!(dev, "G14G receive log record {}: {:02x?}\n", index, &record[..0x20]);
                        let Ok(offset) = crate::m3::payload_offset(thread, slot) else { continue; };
                        let Some(text) = payload.and_then(|p| p.as_slice().get(offset + 0x10..offset + 0xd8))
                            else { continue; };
                        let len = text.iter().position(|v| *v == 0).unwrap_or(text.len());
                        dev_info!(dev, "G14G firmware log[{}]: {}\n", thread,
                                  core::str::from_utf8(&text[..len]).unwrap_or("<non-UTF8>"));
                    } else {
                        dev_info!(dev, "G14G receive {} record {}: {:02x?}\n", name, index, record);
                    }
                }
            }
        }
    }

    /// Only the shared ring and the hardware register are sampled while the ASC runs.
    pub(crate) fn report_power_progress(&self, stage: &str, operating_point: u32) -> Result {
        let globals = self.globals.as_ref().ok_or(EINVAL)?;
        let ring = &self._channels[2 * crate::m3::DEVICE_CONTROL];
        let read = |a: &fw::types::GpuArray<u8>, offset| {
            a.shared_index(offset).map(|v| v.load(Ordering::Acquire))
        };
        dev_info!(self.graph.dev.as_ref(),
                  "G14G {} Fender={:#x} request_pending={} index={} ready={} requested={} assertions host={} fw={} control consumer={} target={} producer={}\n",
                  stage, operating_point, (operating_point >> 4) & 1, operating_point & 0xf,
                  read(&self.status, 0x14)?, read(&self.status, 0x18)?,
                  read(globals, 0xe40)?, read(&self.control, 0xe838)?,
                  read(ring, 0)?, read(ring, 0x10)?, read(ring, 0x20)?);
        dev_info!(self.graph.dev.as_ref(), "G14G {} command service (stage, request)={:?} neighbours={:x?}\n",
                  stage, self.command_service()?,
                  [read(&self.control, 0x4bf8)?, read(&self.control, 0x4bfc)?, read(&self.control, 0x4c04)?,
                   read(&self.control, 0x4c08)?, read(&self.control, 0x4c20)?, read(&self.control, 0x4c28)?]);
        Ok(())
    }

    /// Audit immutable host inputs after the firmware has published ready. The caller
    /// checks the context-0 upper root and supplies the walk of its installed window.
    pub(crate) fn audit_power_inputs(&self, leaf: impl Fn(u64) -> Result<u64>, pa_mask: u64) -> Result {
        use crate::m3::m3_mmio_26::{TABLE_OFFSET, RECORD_BYTES};
        use crate::g17::fw::m5::initdata as abi;
        let word = |a: &fw::types::GpuArray<u8>, n| a.shared_index(n).map(|v| v.load(Ordering::Acquire));
        let pair = |a: &fw::types::GpuArray<u8>, n| -> Result<u64> {
            Ok(u64::from(word(a, n)?) | (u64::from(word(a, n + 4)?) << 32))
        };
        let dir = self.directory.as_ref().ok_or(EINVAL)?;
        let address = pair(dir, 0)?;
        let hw = self._channels.iter().find(|a| a.gpu_va().get() == address).ok_or(EFAULT)?;
        let list = self.boot_page.as_ref().ok_or(EINVAL)?;
        dev_info!(self.graph.dev.as_ref(),
                  "G14G power path perf={} cre={} init_list_first_type={}\n",
                  word(hw, 0xfc0)?, word(hw, 0x26b8)?, word(list, 0x10)?);
        dev_info!(self.graph.dev.as_ref(), "G14G slow={} event_completion={} cre_companion={}\n",
                  word(hw, 0xfc8)?, word(hw, 0x1054)?, word(hw, 0x26bc)?);
        let audit_owner = |owner: &fw::types::GpuArray<u8>| -> Result {
            let va = owner.gpu_va().get();
            let end = va + owner.as_slice().len() as u64 - 1;
            let mut page = va & !(mmu::UAT_PGMSK as u64);
            while page <= end {
                let installed = leaf(page)?;
                let expected = crate::g15::uat::read_leaf(self.graph.uat.kernel_window_root(),
                    page & ((1 << 39) - 1))?.ok_or(EFAULT)?;
                if installed != expected || installed & 3 != 3 { return Err(EFAULT); }
                page += mmu::UAT_PGSZ as u64;
            }
            Ok(())
        };
        audit_owner(dir)?;
        audit_owner(hw)?;
        if crate::module_parameters::t8112_power_filter.value() {
            let controller = self.controller.as_ref().ok_or(EINVAL)?;
            // The directory's immutable controller pointer is packed at an odd offset.
            let target = dir.as_slice().get(0x485..0x48d).ok_or(EINVAL)?;
            if u64::from_le_bytes(target.try_into().map_err(|_| EINVAL)?) != controller.gpu_va().get() {
                return Err(EFAULT);
            }
            audit_owner(controller)?;
            let sample_ticks = word(controller, 4)?;
            let reference_ticks = word(controller, 8)?;
            let samples = word(controller, 0xad8)?;
            let filter_ticks = pair(controller, 0xae0)?;
            let pwr = &self.graph.dyncfg.pwr;
            if sample_ticks == 0 || sample_ticks != pwr.pwr_sample_period_aic_clks ||
                reference_ticks != sample_ticks || samples == 0 ||
                filter_ticks != u64::from(reference_ticks) * u64::from(samples) {
                return Err(EINVAL);
            }
            dev_info!(self.graph.dev.as_ref(),
                      "G14G sampling controller bytes={:#x} period={} reference={} filter_samples={} filter_ticks={} counter_mode={}\n",
                      controller.as_slice().len(), sample_ticks, reference_ticks, samples,
                      filter_ticks, word(controller, 0x6ac8)?);
            // These are firmware statistics outputs. Verify the published pointers and every
            // backing page independently of whether the firmware has produced any records.
            for (at, size) in [(0x1f0, 0x30), (0x1f8, 0x4800)] {
                let va = pair(dir, at)?;
                let owner = self._channels.iter().find(|a| a.gpu_va().get() == va).ok_or(EFAULT)?;
                if owner.as_slice().len() != size { return Err(EINVAL); }
                audit_owner(owner)?;
                dev_info!(self.graph.dev.as_ref(),
                          "G14G statistics directory={:#x} bytes={:#x} mapping verified\n", at, size);
            }
        }
        for slot in 0..60 {
            let at = TABLE_OFFSET + RECORD_BYTES * slot;
            let physical = pair(hw, at)?;
            let va = pair(hw, at + 8)?;
            let bytes = word(hw, at + 16)?;
            let scratch = self.slot_ram.iter().find(|ram| ram.slot == slot);
            if let Some(ram) = scratch {
                if physical != ram.ram.physical() || va != ram.mappings[0].iova()
                    || (bytes as usize).next_multiple_of(mmu::UAT_PGSZ) != ram.initial.len()
                    || word(hw, at + 20)? != bytes || pair(hw, at + 32)? != (if G14SlotRam::readback(slot) { 0 } else { 2 }) {
                    return Err(EFAULT);
                }
            }
            if va == 0 && bytes == 0 && physical == 0 {
                if [0,1,3,8,9,10,12,17,31,32,33,34,37,40,44].contains(&slot) {
                    dev_info!(self.graph.dev.as_ref(), "G14G audit slot={} absent\n", slot);
                }
                continue;
            }
            if bytes == 0 || va == 0 { return Err(EINVAL); }
            dev_info!(self.graph.dev.as_ref(), "G14G audit slot={} descriptor_pa={:#x} va={:#x} bytes={:#x} requested={:#x} mode={}\n",
                      slot, physical, va, bytes, word(hw, at + 20)?, pair(hw, at + 32)?);
            let mut page = va & !(mmu::UAT_PGMSK as u64);
            let end = va.checked_add(bytes.into()).ok_or(EOVERFLOW)?;
            while page < end {
                let pte = leaf(page)?;
                let bits = mmu::Prot::from_pte(pte).leaf_bits();
                if let Some(ram) = scratch {
                    let (expected, prot) = ram.backing((page - va) as usize);
                    if pte & 3 != 3 || pte & pa_mask != expected || bits != prot.leaf_bits() {
                        return Err(EFAULT);
                    }
                }
                let access = if pte & 3 != 3 { "invalid" }
                    else if bits == mmu::PROT_FW_MMIO_RW.leaf_bits() { "RW" }
                    else if bits == mmu::PROT_FW_MMIO_RO.leaf_bits() { "RO" }
                    else if bits == mmu::PROT_FW_SHARED_RW.leaf_bits() { "RAM-RW" }
                    else if bits == mmu::PROT_FW_SHARED_RO.leaf_bits() { "RAM-RO" }
                    else { "other" };
                dev_info!(self.graph.dev.as_ref(), "G14G audit leaf slot={} page={:#x} pte={:#x} pa={:#x} access={}\n",
                          slot, page, pte, pte & pa_mask, access);
                page += mmu::UAT_PGSZ as u64;
            }
        }
        dev_info!(self.graph.dev.as_ref(), "G14G audit OPP ceiling={} hardware={:#x}\n", word(hw,0x10e0)?, address);
        if crate::module_parameters::t8112_power_curve.value() {
            let mut bits = [0u32; 16];
            for (i, value) in bits.iter_mut().enumerate() {
                *value = word(hw, abi::COMMON26_HW_PERF_RELATIVE_A + 4*i)?;
            }
            let ceiling = word(hw, 0x10e0)? as usize;
            if !(1..16).contains(&ceiling) || bits[0] != 0 ||
                bits[ceiling] != 100.0f32.to_bits() ||
                bits[..=ceiling].windows(2).any(|s| s[0] >= s[1]) ||
                bits[ceiling+1..].iter().any(|&x| x != 0) { return Err(EINVAL); }
            dev_info!(self.graph.dev.as_ref(), "G14G calibrated power curve h={} words={:x?} alarm_fields={}\n",
                      ceiling, bits, crate::module_parameters::t8112_alarm_fields.value());
        }
        if crate::module_parameters::t8112_power_filter.value() {
            let pwr = &self.graph.dyncfg.pwr;
            let base = pwr.perf_base_pstate as usize;
            let top = pwr.perf_max_pstate as usize;
            let mut frequency = [0u32; 16];
            for (i, value) in frequency.iter_mut().enumerate() {
                *value = word(hw, abi::COMMON26_HW_PERF_RELATIVE_B + 4*i)?;
            }
            if top >= frequency.len() || base >= top
                || frequency[..=base].iter().any(|&bits| bits != 0)
                || frequency[base..=top].windows(2).any(|v| v[0] >= v[1])
                || frequency[top] != 100.0f32.to_bits()
                || frequency[top+1..].iter().any(|&bits| bits != 0) {
                return Err(EINVAL);
            }
            dev_info!(self.graph.dev.as_ref(), "G14G calibrated frequency curve base={} top={} bits={:x?}\n",
                      base, top, frequency);
        }
        for i in 0..abi::COMMON26_HW_PERF_ROWS {
            let row = i * abi::COMMON26_HW_PERF_ROW_STRIDE;
            let core = abi::COMMON26_HW_PERF_CORE_VOLTAGE + row;
            let sram = abi::COMMON26_HW_PERF_SRAM_VOLTAGE + row;
            let mv = word(hw,core)?;
            let sv = word(hw,sram)?;
            let mut equal = true;
            for lane in 1..abi::COMMON26_HW_PERF_LANES {
                equal &= word(hw, core+4*lane)? == mv && word(hw, sram+4*lane)? == sv;
            }
            dev_info!(self.graph.dev.as_ref(), "G14G audit OPP row={} MHz={} mV={} SRAM_mV={} lanes_equal={}\n",
                      i, word(hw, abi::COMMON26_HW_PERF_FREQUENCY + 4*i)?, mv, sv, equal);
        }
        Ok(())
    }

    /// The ASC has stopped and the host has not modified these records since publication.
    pub(crate) fn report_stopped_power(&self) -> Result {
        for ram in &self.slot_ram {
            ram.report_stopped(&self.graph)?;
        }
        let directory = self.directory.as_ref().ok_or(EINVAL)?;
        let controller = self.controller.as_ref().ok_or(EINVAL)?;
        let read = |a: &fw::types::GpuArray<u8>, offset| -> Result<u32> {
            let word = a.shared_index(offset)?;
            crate::pgtable::clean_invalidate_poc(word.as_ptr().cast(), 4);
            crate::pgtable::poc_barrier();
            Ok(word.load(Ordering::Acquire))
        };
        dev_info!(self.graph.dev.as_ref(),
                  "G14G stopped power request_marker={} source_states=[{},{}] selector={} current={} input={} power_up_count={}\n",
                  read(directory, 0x330)?, read(controller, 0x2c)?, read(controller, 0x30)?,
                  read(controller, 0x34)?, read(controller, 0x38)?, read(controller, 0xa4)?,
                  read(controller, 0x6bd4)?);
        if crate::module_parameters::t8112_power_filter.value() {
            dev_info!(self.graph.dev.as_ref(),
                "G14G stopped controller sample={} budget={} peak_state={} average_state={} effective={}\n",
                read(controller, 0x218)?, read(controller, 0xacc)?, read(controller, 0xb00)?,
                read(controller, 0x2a64)?, read(controller, 0x50)?);
            dev_info!(self.graph.dev.as_ref(),
                "G14G thermal at stop active={} temperature_cc={} target_cc={} effort_scale_bits={:#x} request={}\n",
                read(controller, 0x2270)?, read(controller, 0x2274)?, read(controller, 0x22d4)?,
                read(controller, 0x22b0)?, read(controller, 0x2308)?);
            dev_info!(self.graph.dev.as_ref(),
                "G14G stopped demand deadline={} perf={} se={} pmu_limit={}\n",
                read(controller, 0x2220)?, read(controller, 0xbd0)?, read(controller, 0x2bf4)?,
                read(controller, 0x2cb0)?);
        }
        Ok(())
    }

    /// Discriminators for a parked job. Safe while the ASC runs: host cache
    /// lines are refreshed before each read and nothing is written.
    /// Firmware-visible addresses of the trial job's host objects, for the private scan.
    pub(crate) fn job_anchors(&self) -> KVec<(u64, &'static str)> {
        let mut v = KVec::new();
        if let Some(job) = &self.startup_fence {
            let q = job.queue.gpu_va().get();
            for item in [(q, "queue"), (q + 0x90, "records"), (q + 0xe8, "record1"),
                         (job._owners[2].gpu_va().get(), "scheduler"),
                         (job._owners[3].gpu_va().get(), "command")] {
                let _ = v.push(item, GFP_KERNEL);
            }
        }
        v
    }

    pub(crate) fn report_job_gates(&self, stage: &str) -> Result {
        let dev = self.graph.dev.as_ref();
        let read = |a: &fw::types::GpuArray<u8>, offset: usize| -> Result<u32> {
            let word = a.shared_index(offset)?;
            crate::pgtable::clean_invalidate_poc(word.as_ptr().cast(), 4);
            crate::pgtable::poc_barrier();
            Ok(word.load(Ordering::Acquire))
        };
        if let Some(job) = self.startup_fence.as_ref() {
            for record in [0x90usize, 0xe8, 0x140] {
                let q = &job.queue;
                dev_info!(dev, "G14G {} work record +{:#x}: state={} flags={:#x} command={:#x}{:08x} cursor={:#x}{:08x} mask={:#x} token={:#x}{:08x}/{:#x}\n",
                          stage, record, read(q, record)?, read(q, record + 4)?,
                          read(q, record + 0x14)?, read(q, record + 0x10)?,
                          read(q, record + 0x34)?, read(q, record + 0x30)?, read(q, record + 0x40)?,
                          read(q, record + 0x4c)?, read(q, record + 0x48)?, read(q, record + 0x50)?);
            }
            if let Some((_, _, ts)) = job._client.as_ref() {
                dev_info!(dev, "G14G {} latch=[{}, {}, {:#x}, {:#x}] queue group={}/{} sub={} lane={:#x}\n", stage,
                          read(ts, 0x20)?, read(ts, 0x24)?, read(ts, 0x28)?, read(ts, 0x2c)?,
                          read(&job.queue, 0x30)?, read(&job.queue, 0x34)?, read(&job.queue, 0x44)?, read(&job.queue, 0x48)?);
            }
        }
        if let Some(c) = self.controller.as_ref() {
            dev_info!(dev, "G14G {} controller achieved={} effective={} sample={} zones={} average={} ticks={} hist=[{:#x}, {:#x}] override={} S.zones={} S.pending={}\n",
                      stage, read(c, 0x38)?, read(c, 0x50)?, read(c, 0x218)?, read(c, 0x28e8)?, read(c, 0x2a64)?,
                      read(c, 0x2a2c)?, read(c, 0x2a58)?, read(c, 0x2a5c)?, read(c, 0x2a68)?,
                      read(&self.power, 0x0c)?, read(&self.power, 0x08)?);
            let stats = &self._channels[2 * 16];
            dev_info!(dev, "G14G {} live temperature_cc={} power_ups={} stats read={} write={} events write={}\n",
                      stage, read(c, 0x2274)?, read(c, 0x6bd4)?, read(stats, 0)?, read(stats, 0x20)?,
                      read(&self._channels[2 * 13], 0x20)?);
        }
        Ok(())
    }

    pub(crate) fn report_stopped_fence(&self) {
        if let Some(job) = &self.startup_fence {
            let read = |buf: &fw::types::GpuArray<u8>, at: usize| {
                let b = buf.as_slice();
                u32::from_le_bytes([b[at], b[at+1], b[at+2], b[at+3]])
            };
            let q = job.queue.as_slice();
            crate::pgtable::clean_invalidate_poc(q.as_ptr(), 0x90);
            crate::pgtable::poc_barrier();
            dev_info!(self.graph.dev.as_ref(), "G14G queue mapping check passed; stamp slot={} TX producer={} queue producer={} accepted={} first={} retired={}\n",
                      job.event.slot(), read(&self._channels[2 * Self::signal_ring()], 0x20), read(&job.positions, 0x40),
                      read(&job.queue, 0x28), read(&job.queue, 0x58), read(&job.positions, 0));
            dev_info!(self.graph.dev.as_ref(), "G14G first fence channel read={} queue read={} counter={} stamp={} events={}\n",
                      read(&self._channels[2 * Self::signal_ring()], 0), read(&job.positions, 0x30),
                      u64::from(read(&job.counter, 0)) | (u64::from(read(&job.counter, 4)) << 32),
                      job.event.current().raw(),
                      read(&self._channels[26], 0x20));
            if read(&self._channels[26], 0x20) != 0 {
                let event = &self._channels[27].as_slice()[..0x48];
                dev_info!(self.graph.dev.as_ref(), "G14G first firmware event {:02x?}\n", event);
            }
            // Exact bytes sent, and the queue state as the stopped firmware left it.
            // Owners are [timestamps, pointers, scheduler, command, stream].
            let dev = self.graph.dev.as_ref();
            let dump = |name: &str, va: u64, bytes: &[u8]| {
                for (row, chunk) in bytes.chunks(32).enumerate() {
                    if chunk.iter().any(|v| *v != 0) {
                        dev_info!(dev, "G14G sent {} {:#x}+{:#05x}: {:02x?}\n", name, va, row * 32, chunk);
                    }
                }
            };
            for owner in job._owners.iter() {
                crate::pgtable::clean_invalidate_poc(owner.as_slice().as_ptr(), owner.as_slice().len());
            }
            crate::pgtable::clean_invalidate_poc(q.as_ptr(), 0x400);
            crate::pgtable::poc_barrier();
            let ring = &self._channels[2 * Self::signal_ring() + 1];
            dump("submission", ring.gpu_va().get(), &ring.as_slice()[..0x18]);
            dump("command", job._owners[3].gpu_va().get(), &job._owners[3].as_slice()[..0x360]);
            if let Some((_, _, ts)) = job._client.as_ref() { dump("jobtime", ts.gpu_va().get(), ts.as_slice()); }
            if let Some(dir) = self.directory.as_ref() {
                for at in [0x264usize, 0x26c] {
                    let va = u64::from_le_bytes(dir.as_slice()[at..at + 8].try_into().unwrap_or([0; 8]));
                    if let Some(owner) = self._channels.iter().find(|a| a.gpu_va().get() == va) {
                        crate::pgtable::clean_invalidate_poc(owner.as_slice().as_ptr(), owner.as_slice().len());
                        dump(if at == 0x264 { "compute-state" } else { "scheduler-state" }, va, owner.as_slice());
                    }
                }
            }
            dump("stream", job._owners[4].gpu_va().get(), job._owners[4].as_slice());
            dump("pointers", job._owners[1].gpu_va().get(), &job._owners[1].as_slice()[..0x20]);
            dump("scheduler", job._owners[2].gpu_va().get(), job._owners[2].as_slice());
            dump("timestamps", job._owners[0].gpu_va().get(), job._owners[0].as_slice());
            dump("positions", job.positions.gpu_va().get(), &job.positions.as_slice()[..0x60]);
            dump("queue", job.queue.gpu_va().get(), &q[..0x400]);
            dump("tracking", job._owners[5].gpu_va().get(), job._owners[5].as_slice());
            dump("cursor", job._owners[6].gpu_va().get(), job._owners[6].as_slice());
        }
    }

    /// Read the uncached shared status after the ASC has stopped.
    pub(crate) fn stopped_status(&self) -> (u32, u32, u32) {
        let data = self.status.as_slice();
        let word = |i: usize| u32::from_le_bytes([data[i], data[i+1], data[i+2], data[i+3]]);
        (word(4), word(0x10), word(0x14))
    }

    pub(crate) fn build(
        graph: &Arc<GpuManagerG14V13_5>,
        word: u64,
        directory_layout: Option<G14DirectoryHeader>,
    ) -> Result<fw::types::GpuObject<Self>> {
        use crate::m3::m3_init_26 as extended;
        let mut alloc = graph.alloc();
        let mut startup_fence = if matches!(directory_layout, Some(G14DirectoryHeader::J413 { first_job: true, .. })) {
            Some(G14StartupFence::new(&mut alloc, graph.event_manager.try_get_polled()?, graph.trial_uat(), &graph.dev)?)
        } else {
            None
        };
        let mut channels = KVec::new();
        let mut controller = None;
        let mut slot_ram = KVec::new();
        let mut register_mappings = KVec::new();
        let board = matches!(directory_layout, Some(G14DirectoryHeader::J413 { .. }));
        let power_filter = crate::module_parameters::t8112_power_filter.value();
        let pwr = &graph.dyncfg.pwr;
        if power_filter && (!board || !crate::module_parameters::t8112_power_curve.value()
            || crate::module_parameters::t8112_alarm_fields.value() != 13
            || !pwr.power_zones.is_empty() || pwr.max_power_mw == 0
            || pwr.perf_states.last().map(|s| s.pwr_mw) != Some(pwr.max_power_mw)
            || pwr.max_power_mw > i32::MAX as u32 || pwr.power_sample_period == 0
            || pwr.ppm_filter_time_constant_ms < pwr.power_sample_period
            || pwr.avg_power_filter_tc_ms < pwr.power_sample_period
            || pwr.avg_power_target_filter_tc == 0) {
            return Err(EINVAL);
        }
        // On G14G the 0x60-byte root child and the eight-byte temperature
        // generation occupy consecutive, 64-byte-aligned shared suballocations.
        let board_extra = if board {
            Some(alloc.shared.array_empty::<u8>(0x80 + 8)?)
        } else {
            None
        };
        let directory = if let Some(header) = directory_layout {
            let mut addresses = [0; 34];
            for (index, address) in addresses.iter_mut().enumerate() {
                let size = extended::allocation(index).map_err(|_| EINVAL)?.size;
                let owner = if board && index < 26 && index % 2 == 1 {
                    alloc.private.array_empty::<u8>(size)?
                } else {
                    alloc.shared.array_empty::<u8>(size)?
                };
                *address = owner.gpu_va().get();
                channels.push(owner, GFP_KERNEL)?;
            }
            if let Some(job) = startup_fence.as_ref() {
                // Compute at priority zero uses the third TX descriptor.
                let slot = u8::try_from(job.event.slot()).map_err(|_| EINVAL)?;
                let engine = Self::signal_engine();
                let entry = crate::m3::submission_entry(engine as u32, job.queue.gpu_va().get(), 1, slot,
                        crate::module_parameters::t8112_first_publication.value())
                    .map_err(|_| EINVAL)?;
                // The ring starts empty at index R (consumer = intermediate = producer = R).
                let r = Self::ring_start() as usize;
                let n = entry.len();
                let ring = channels[2 * Self::signal_ring() + 1].as_mut_slice();
                ring[r * n..(r + 1) * n].copy_from_slice(&entry);
                if job.second.is_some() {
                    let next = crate::m3::submission_entry(engine as u32, job.queue.gpu_va().get(), 2, slot, false)
                        .map_err(|_| EINVAL)?;
                    let r2 = (r + 1) % crate::m3::m3_channel_format::SLOTS as usize;
                    ring[r2 * n..(r2 + 1) * n].copy_from_slice(&next);
                }
                crate::pgtable::clean_poc(ring.as_ptr(), ring.len());
                let state = channels[2 * Self::signal_ring()].as_mut_slice();
                for at in [0usize, 0x10, 0x20] {
                    state[at..at + 4].copy_from_slice(&(r as u32).to_le_bytes());
                }
                crate::pgtable::clean_poc(state.as_ptr(), state.len());
            }
            if startup_fence.is_some() && crate::module_parameters::t8112_handshake.value() == 14
                && crate::module_parameters::t8112_idle_policy.value() {
                // Reuse the J613 extended order: opening record, two engine classes
                // per data master, then the counted idle-policy records.
                let base = Self::control_base() as usize;
                let records = &mut channels[2 * crate::m3::DEVICE_CONTROL + 1].as_mut_slice()
                    [..(base + 2) * crate::m3::CONTROL_BYTES];
                for (index, record) in records.chunks_exact_mut(crate::m3::CONTROL_BYTES).enumerate() {
                    let action = if index >= base {
                        crate::m3::Action::Idle { enabled: index == base + 1 }
                    } else if index == 0 {
                        crate::m3::Action::Initialize
                    } else {
                        let ordinal = index as u32 - 1;
                        crate::m3::Action::Register { pipe: ordinal / 2, engine: ordinal % 2 + 1 }
                    };
                    action.encode(record).map_err(|_| EINVAL)?;
                    if index != 0 && index < base {
                        record[..4].copy_from_slice(
                            &crate::module_parameters::t8112_class_opcode.value().to_le_bytes());
                    }
                    if index == 0 && index < base {
                        // The 27.x firmware line renumbered the opening handler (G18G 26A428
                        // moved it from 0x16 to 0x15): the record body is unchanged.
                        record[..4].copy_from_slice(
                            &crate::module_parameters::t8112_opening_opcode.value().to_le_bytes());
                    }
                }
                crate::pgtable::clean_poc(records.as_ptr(), records.len());
                if base != 0 {
                    // As on J613, the opening record is pending when the root becomes
                    // visible and retires through InitData consumption without a kick.
                    let state = channels[2 * crate::m3::DEVICE_CONTROL].as_mut_slice();
                    state[0x20..0x24].copy_from_slice(&1u32.to_le_bytes());
                    crate::pgtable::clean_poc(state.as_ptr(), state.len());
                }
            }
            let mut directory = if board { &mut alloc.private } else { &mut alloc.shared }
                .array_empty::<u8>(0x4c8)?;
            let bytes = directory.as_mut_slice();
            extended::write_channels(bytes, &addresses).map_err(|_| EINVAL)?;
            let base = &graph.initdata.runtime_pointers;
            bytes[..8].copy_from_slice(&base.hwdata_b.gpu_va().get().to_le_bytes());
            bytes[0x10..0x18].copy_from_slice(&base.unkptr_190.gpu_va().get().to_le_bytes());
            if let G14DirectoryHeader::J413 { sgx, sgx_size, interrupt, copy_bytes, live, .. } = header {
                use crate::m3::m3_mmio_26::{write_record, RECORD_BYTES, TABLE_OFFSET};
                use fw::initdata::raw::HwDataBG14V27_2 as Hw;
                let mut hardware = alloc.private.array_empty::<u8>(0x2788)?;
                let hw = hardware.as_mut_slice();
                let scratch_pa = sgx + 0xe10000;
                let mut io_va = IOVA_KERN_MMIO_RANGE.start;
                let sgx_end = sgx.checked_add(sgx_size).ok_or(EOVERFLOW)?;
                // Some disabled records retain their mode even with no backing.
                let telemetry_ro = live && crate::module_parameters::t8112_telemetry_ro.value();
                // Stock maps the real 0x1000-byte push-telemetry window RW. The PMP
                // takes the GPU's performance-state reports from it.
                let real_dashboard = live && crate::module_parameters::t8112_dashboard_real.value();
                let ram_mask = crate::module_parameters::t8112_ram_slots.value();
                let trial_mask = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 8) | (1 << 10)
                    | (1 << 12) | (1 << 17) | (1 << 33) | (1 << 40) | (1 << 44);
                if ram_mask & !trial_mask != 0 { return Err(EINVAL); }
                let slot_mode = |slot: usize| -> u64 {
                    match slot {
                        9 if telemetry_ro && !real_dashboard => 0,
                        0 | 2 | 3 | 5..=9 | 12 | 17 | 25..=27 | 31..=40 |
                        42..=48 | 52..=55 | 58 => 2,
                        _ => 0,
                    }
                };
                for slot in 0..60 {
                    let offset = TABLE_OFFSET + RECORD_BYTES * slot;
                    write_record(&mut hw[offset..], 0, 0, 0, 0, 0, slot_mode(slot))
                        .map_err(|_| EINVAL)?;
                }
                for (slot, physical, size, relative) in [
                    (0, interrupt & !0x3fff, 0x4000, 0),
                    (1, 0x23b2c0000, 0x1000, 0),
                    (3, 0x200f04000, 0x18000, 0),
                    (8, 0x27d050000, 0x4000, 0),
                    (9, 0x23b3d0000, 0x1000, 0),
                    (10, 0x23b3c0000, 0x1000, 0),
                    (12, 0x26b45c000, 0x4000, 0),
                    (17, sgx, 0x20000, 0),
                    (31, sgx + 0xd04000, 0xc000, 0xd04000),
                    (32, sgx + 0xd61000, 0x1000, 0xd61000),
                    (33, sgx + 0xe0c000, 0x4000, 0xe0c000),
                    (34, sgx + 0xd10000, 0x4000, 0xd10000),
                    (37, scratch_pa, 0x1000, 0xe10000),
                    (40, sgx + 0xd00000, 0x4000, 0xd00000),
                    (44, sgx + 0xd80000, 0x8000, 0),
                ] {
                    let replace = ram_mask & (1u64 << slot) != 0;
                    let dashboard = slot == 9 && crate::module_parameters::t8112_dashboard_ram.value() && !real_dashboard;
                    if live && (replace || dashboard) {
                        if !telemetry_ro || crate::module_parameters::t8112_handshake.value() != 14 {
                            return Err(EINVAL);
                        }
                        // Independent trial VAs leave every existing register VA unchanged.
                        // The real update aperture and external MMIO are never installed here.
                        let span = (size as usize).next_multiple_of(mmu::UAT_PGSZ);
                        let offset = if dashboard { 0x10_0000 } else { 0x20_0000 + slot as u64 * 0x2_0000 };
                        let va = if matches!(slot, 1 | 10 | 17 | 33 | 40 | 44) {
                            let original = io_va;
                            io_va = io_va.checked_add(span as u64).ok_or(EOVERFLOW)?;
                            original
                        } else {
                            IOVA_KERN_MMIO_RANGE.start + offset
                        };
                        let ram = G14SlotRam::new(graph, slot, span, va, physical)?;
                        write_record(&mut hw[TABLE_OFFSET + RECORD_BYTES * slot..],
                                     ram.ram.physical(), ram.mappings[0].iova(), size, size, relative, if G14SlotRam::readback(slot) { 0 } else { 2 })
                            .map_err(|_| EINVAL)?;
                        slot_ram.push(ram, GFP_KERNEL)?;
                        dev_info!(graph.dev.as_ref(), "G14G scratch slot={} installed={} logical={}\n", slot, span, size);
                        continue;
                    }
                    let subpage = physical as usize & mmu::UAT_PGMSK;
                    let span = (subpage + size as usize + mmu::UAT_PGMSK) & !mmu::UAT_PGMSK;
                    let writable = slot_mode(slot) == 2;
                    let page_start = physical - subpage as u64;
                    let page_end = page_start.checked_add(span as u64).ok_or(EOVERFLOW)?;
                    // The admission-only profile can describe shared SoC registers.
                    // On live bring-up give write access only to full pages owned by SGX.
                    if live && writable && (page_start < sgx || page_end > sgx_end) && !(real_dashboard && slot == 9) {
                        continue;
                    }
                    // Place the optional read-only mapping after the fixed SGX register set
                    // so every other register keeps the same VA as the control.
                    let separate = telemetry_ro && slot == 9;
                    let va = if separate {
                        IOVA_KERN_MMIO_RANGE.start + 0x10_0000
                    } else {
                        io_va
                    };
                    let page = graph.uat.kernel_vm().map_io(
                        graph.dev.as_ref().as_ref(), va, page_start as usize, span,
                        if writable { mmu::PROT_FW_MMIO_RW } else { mmu::PROT_FW_MMIO_RO },
                    )?;
                    if page.translate_offset(subpage)? != physical
                        || page.translate_offset(subpage + size as usize - 1)? != physical + u64::from(size) - 1 {
                        return Err(EINVAL);
                    }
                    write_record(&mut hw[TABLE_OFFSET + RECORD_BYTES * slot..], physical,
                        page.iova() + subpage as u64, size, size, relative,
                        slot_mode(slot)).map_err(|_| EINVAL)?;
                    if !separate {
                        io_va = io_va.checked_add(span as u64).ok_or(EOVERFLOW)?;
                    }
                    register_mappings.push(page, GFP_KERNEL)?;
                }
                let dump = core::mem::offset_of!(Hw, coredump_copy_buffer_size);
                hw[dump..dump+8].copy_from_slice(&u64::from(copy_bytes).to_le_bytes());
                base.hwdata_b.with(|old, _| {
                    for (offset, value) in [
                        (0x00, old.unk_8.0), (0x08, old.unk_18.0),
                        (0x10, 0x11_00000000), (0x18, 0x11_00000000),
                        (0x20, 0x6f_ffff8000), (0x28, G14G_TIMESTAMP_RANGE.start),
                        (0x10cc, old.uat_ttb_base.0),
                    ] {
                        hw[offset..offset+8].copy_from_slice(&value.to_le_bytes());
                    }
                    for (offset, value) in [(0x10d4, old.gpu_core_id), (0x10d8, old.gpu_rev_id),
                                            (0x10dc, graph.dyncfg.id.total_active_cores), (0x10e0, old.max_pstate)] {
                        hw[offset..offset+4].copy_from_slice(&value.to_le_bytes());
                    }
                    // Legacy J413 HwDataB configuration carried to the 26B123 inline block. The
                    // chip words (legacy +0x450 -> +0xfb0) and the two words already confirmed on
                    // hardware (+0x460 -> +0xfc0, +0x4f4 -> +0x1054) give a shift of 0xb60; the
                    // firmware's own read list places unk_534 and num_frags at +0x109c/+0x10a4.
                    let legacy = crate::module_parameters::t8112_hw_legacy.value();
                    let mut carry = |bit: u64, offset: usize, value: u32| {
                        if legacy & bit != 0 { hw[offset..offset + 4].copy_from_slice(&value.to_le_bytes()); }
                    };
                    carry(1, 0x10a4, old.num_frags);
                    carry(2, 0x1064, old.unk_504);
                    carry(4, 0x1020, old.unk_4c0);
                    carry(8, 0x109c, old.unk_534);
                    carry(0x10, 0x1084, old.unk_524);
                    for (offset, value) in [(0xfc4, old.unk_464), (0xfdc, old.unk_47c), (0xfe4, old.unk_484),
                                            (0xfec, old.unk_48c), (0xff0, old.base_clock_khz),
                                            (0xff4, old.power_sample_period), (0xffc, old.unk_49c),
                                            (0x1000, old.unk_4a0), (0x1004, old.unk_4a4)] {
                        carry(0x20, offset, value);
                    }
                    // Inline words whose stock J413 value the host producer fixes.
                    // These replace the legacy-shift guesses for the same offsets.
                    for (offset, value) in [(0xfc8u32, 0u32), (0xfd8, 1), (0xfec, 24000), (0xff0, 1),
                                            (0xffc, 1), (0x2724, 1), (0x2754, 1)] {
                        carry(0x40, offset as usize, value);
                    }
                    // Capability words {8,10,11,18,24,25,53} for a gpu,t8112
                    // node without model-slow. They replace the legacy carry at the same offsets.
                    for (offset, value) in [(0xfc4u32, 1u32), (0xfdc, 1), (0xfe0, 0), (0x1028, 0),
                                            (0x109c, 0), (0x1064, 0), (0x2728, 0)] {
                        carry(0x80, offset as usize, value);
                    }
                    // Compute resource mask, one low bit per configured core.
                    // The firmware imports it and seeds the compute engine's population from it.
                    let cores = graph.dyncfg.id.total_active_cores;
                    carry(0x100, 0x2670, if cores < 32 { (1u32 << cores) - 1 } else { u32::MAX });
                    let chip = core::mem::offset_of!(Hw, chip_id);
                    for (i, word) in [old.chip_id, old.unk_454, old.unk_458, old.unk_45c]
                        .into_iter().enumerate() {
                        let offset = chip + 4 * i;
                        hw[offset..offset + 4].copy_from_slice(&word.to_le_bytes());
                    }
                });
                // G15G 26.6.2 and the common newer profile agree on the first table:
                // 16 lanes of voltage per state and frequencies expressed in MHz.
                use crate::g17::fw::m5::initdata as abi;
                let mut sram_table = None;
                let mut found_sgx = false;
                let chosen = kernel::of::chosen().ok_or(ENODEV)?;
                for parent in chosen.children() {
                    let name = parent.get_opt_property::<KVec<u8>>(c"name")?;
                    if name.as_deref() != Some(b"aurora-gpu-resources\0") &&
                        name.as_deref() != Some(b"aurora-gpu-resources") {
                        continue;
                    }
                    for child in parent.children() {
                        let name = child.get_opt_property::<KVec<u8>>(c"name")?;
                        if name.as_deref() == Some(b"sgx\0") || name.as_deref() == Some(b"sgx") {
                            found_sgx = true;
                            sram_table = child.get_opt_property::<KVec<u8>>(c"adt-perf-states-sram")?;
                        }
                    }
                }
                let perf = crate::module_parameters::t8112_performance_control.value();
                let cre = crate::module_parameters::t8112_cre.value();
                let event = crate::module_parameters::t8112_event_completion.value();
                let base_request = crate::module_parameters::t8112_base_request.value();
                let curve = crate::module_parameters::t8112_power_curve.value();
                if perf || cre || event || base_request {
                    if !live || !telemetry_ro || ram_mask & !(1 << 17) != 0 ||
                        crate::module_parameters::t8112_handshake.value() != 14 ||
                        !(curve || crate::module_parameters::t8112_idle_policy.value()) { return Err(EINVAL); }
                    if (event || base_request) && !(perf && cre) { return Err(EINVAL); }
                    for (offset, enabled) in [(0xfc0, perf), (0x26b8, cre), (0x1054, event)] {
                        hw[offset..offset+4].copy_from_slice(&u32::from(enabled).to_le_bytes());
                    }
                }
                let id = &graph.dyncfg.id;
                let [mask] = id.core_masks.as_slice() else { return Err(EINVAL); };
                if mask.count_ones() != id.total_active_cores || id.total_active_cores == 0 {
                    return Err(EINVAL);
                }
                hw[0x267c..0x2684].copy_from_slice(&u64::from(*mask).to_le_bytes());
                hw[0x2684..0x2688].copy_from_slice(&id.total_active_cores.to_le_bytes());
                let states = &graph.dyncfg.pwr.perf_states;
                if !found_sgx || states.is_empty() || states.len() > abi::COMMON26_HW_PERF_ROWS ||
                    sram_table.as_ref().is_some_and(|table| table.len() != states.len()*8) {
                    return Err(EINVAL);
                }
                let alarm_fields = crate::module_parameters::t8112_alarm_fields.value() as usize;
                let inventory = crate::module_parameters::t8112_calibrated_sensors.value();
                let fast_selection = graph.cfg.fast_sensor_mask[0];
                if curve {
                    if !base_request || !(2..=16).contains(&states.len()) ||
                        !crate::module_parameters::t8112_dashboard_ram.value() ||
                        !crate::module_parameters::t8112_mtr_sensors.value() ||
                        inventory & fast_selection != fast_selection || inventory >> 32 != 0 ||
                        states.windows(2).any(|pair| pair[1].pwr_mw <= pair[0].pwr_mw) {
                        return Err(EINVAL);
                    }
                    let ceiling = u32::from_le_bytes(hw[0x10e0..0x10e4].try_into().map_err(|_| EINVAL)?);
                    if ceiling as usize != states.len()-1 { return Err(EINVAL); }
                    let baseline = states[0].pwr_mw;
                    let span = crate::float::F32::from(states[ceiling as usize].pwr_mw - baseline);
                    for (i, state) in states.iter().enumerate() {
                        let percentage = crate::float::F32::from(state.pwr_mw - baseline) *
                            crate::f32!(100.0) / span;
                        let at = abi::COMMON26_HW_PERF_RELATIVE_A + i * 4;
                        hw[at..at+4].copy_from_slice(&percentage.to_bits().to_le_bytes());
                    }
                }
                if power_filter {
                    // Performance and SE interpolate frequency above base, not absolute power.
                    // Same normalization as the common26 frequency table on the M5 profile.
                    let base_idx = pwr.perf_base_pstate as usize;
                    let base_hz = states.get(base_idx).ok_or(EINVAL)?.freq_hz;
                    let top_hz = states.last().ok_or(EINVAL)?.freq_hz;
                    if top_hz <= base_hz || states.windows(2).any(|v| v[0].freq_hz >= v[1].freq_hz) {
                        return Err(EINVAL);
                    }
                    let span = crate::float::F32::from(top_hz - base_hz);
                    for (i, state) in states.iter().enumerate().skip(base_idx + 1) {
                        let at = abi::COMMON26_HW_PERF_RELATIVE_B + 4 * i;
                        let relative = crate::float::F32::from(state.freq_hz - base_hz)
                            * crate::f32!(100.0) / span;
                        hw[at..at+4].copy_from_slice(&relative.to_bits().to_le_bytes());
                    }
                }
                if alarm_fields > 13 || (alarm_fields != 0 && !curve) { return Err(EINVAL); }
                for (i,state) in states.iter().enumerate() {
                    let [mv] = state.volt_mv.as_slice() else { return Err(EINVAL); };
                    let sram_mv = if let Some(table) = &sram_table {
                        let (freq, voltage) = table[i*8..i*8+8].split_at(4);
                        if u32::from_le_bytes(freq.try_into().map_err(|_| EINVAL)?) != state.freq_hz {
                            return Err(EINVAL);
                        }
                        let value = u32::from_le_bytes(voltage.try_into().map_err(|_| EINVAL)?);
                        if value < *mv || value > 1100 { return Err(EINVAL); }
                        value
                    } else {
                        (*mv).max(850)
                    };
                    let mhz = (state.freq_hz / 1_000_000).to_le_bytes();
                    for series in [abi::COMMON26_HW_PERF_FREQUENCY, 0x1924] {
                        let freq = series + i * 4;
                        hw[freq..freq+4].copy_from_slice(&mhz);
                    }
                    for lane in 0..abi::COMMON26_HW_PERF_LANES {
                        let row = i*abi::COMMON26_HW_PERF_ROW_STRIDE + lane*4;
                        let core = abi::COMMON26_HW_PERF_CORE_VOLTAGE + row;
                        let sram = abi::COMMON26_HW_PERF_SRAM_VOLTAGE + row;
                        hw[core..core+4].copy_from_slice(&mv.to_le_bytes());
                        hw[sram..sram+4].copy_from_slice(&sram_mv.to_le_bytes());
                    }
                }
                dev_info!(graph.dev.as_ref(), "G14G {} checked MMIO translations; {} operating points, boot SRAM table={}\n",
                    register_mappings.len(), states.len(), sram_table.is_some());
                crate::pgtable::clean_poc(hw.as_ptr(), hw.len());
                bytes[..8].copy_from_slice(&hardware.gpu_va().get().to_le_bytes());
                channels.push(hardware, GFP_KERNEL)?;
                for (offset, size) in [(0x10,0x88),(0x254,0x678),(0x25c,0x760),
                                       (0x264,0x6d0),(0x26c,0x60),(0x485,0x6c48)] {
                    let mut owner = alloc.private.array_empty::<u8>(size)?;
                    if offset == 0x485 {
                        let pwr = &graph.dyncfg.pwr;
                        let policy = owner.as_mut_slice();
                        let max_state = (pwr.perf_states.len() as u32 - 1) * 100;
                        for offset in [0x222c, 0x2880] {
                            policy[offset..offset + 4].copy_from_slice(&max_state.to_le_bytes());
                        }
                        let extra = board_extra.as_ref().ok_or(EINVAL)?;
                        let generation = extra.gpu_va().get() + 0x80;
                        if generation & 0x3f != 0 || extra.as_slice()[0x80..] != [0; 8] {
                            return Err(EINVAL);
                        }
                        policy[0x3b98..0x3ba0].copy_from_slice(&generation.to_le_bytes());
                        let fast_die_fields = crate::module_parameters::t8112_fast_die_fields.value() as usize;
                        if alarm_fields != 0 && fast_die_fields != 4 { return Err(EINVAL); }
                        if fast_die_fields > 4 || (fast_die_fields != 0 && !crate::module_parameters::t8112_mtr_sensors.value()) {
                            return Err(EINVAL);
                        }
                        if crate::module_parameters::t8112_mtr_sensors.value() {
                            if !(base_request && event && crate::module_parameters::t8112_dashboard_ram.value())
                                || ram_mask != 0 { return Err(EINVAL); }
                            // J413's old ABI uses the board alarm mask as its alternate sensor mask.
                            // The newer secure controller accepts a 64-bit sensor set at +0x3ba0.
                            let sensors = if curve { inventory } else { graph.cfg.fast_sensor_mask_alt[0] };
                            if sensors == 0 || sensors >> 32 != 0 { return Err(EINVAL); }
                            policy[0x3ba0..0x3ba8].copy_from_slice(&sensors.to_le_bytes());
                            dev_info!(graph.dev.as_ref(), "G14G secure metrology sensors={:#x}\n", sensors);
                            // Keep the J413 13.5 producer and its board-specific units. These four
                            // J613 sources share a checked relocation band with both J413 OPP anchors.
                            let (release, mask, integral, proportional) = base.hwdata_a.with(|old, _|
                                (old.fast_die0_release_temp_cc, old.fast_die0_sensor_mask.0,
                                 old.fast_die0_ki_dt.to_bits(), old.fast_die0_kp.to_bits()));
                            let fields = [
                                (0x2270, u64::from(release), 4),
                                (0x2268, mask, 8),
                                (0x2294, u64::from(integral), 4),
                                (0x22a4, u64::from(proportional), 4),
                            ];
                            for &(source, value, width) in &fields[..fast_die_fields] {
                                let at = abi::relocate_by_bands(abi::COMMON26_POLICY_BANDS, source).ok_or(EINVAL)?;
                                policy.get_mut(at..at + width).ok_or(EINVAL)?
                                    .copy_from_slice(&value.to_le_bytes()[..width]);
                                dev_info!(graph.dev.as_ref(), "G14G J413 fast-die policy offset={:#x} bytes={} value={:#x}\n",
                                          at, width, value);
                            }
                        }
                        // Each step adds one stock controller field to the same matched profile.
                        // Install thresholds and correction tables before selecting the SoC-hot alarm.
                        let settings: &[(usize, u32)] = &[(0x22d4, 11000), (0x2284, 900), (0x2288, 4),
                            (0x229c, crate::f32!(1.0).to_bits()), (0x2f30, 125), (0x2f34, 1)];
                        for &(at, value) in settings.iter().take(alarm_fields) {
                            policy[at..at+4].copy_from_slice(&value.to_le_bytes());
                        }
                        for &at in [0x2320, 0x2420, 0x2f38, 0x3038].iter().take(alarm_fields.saturating_sub(6)) {
                            for sid in 0..64 {
                                if fast_selection & (1u64 << sid) != 0 {
                                    let field = at + 4 * sid;
                                    policy[field..field+4].copy_from_slice(&crate::f32!(5.3).to_bits().to_le_bytes());
                                }
                            }
                        }
                        if alarm_fields >= 11 {
                            policy[0x2f28..0x2f30].copy_from_slice(&(inventory & fast_selection).to_le_bytes());
                        }
                        if alarm_fields >= 12 {
                            policy[0x3ba8..0x3bac].copy_from_slice(&1u32.to_le_bytes());
                        }
                        if alarm_fields >= 13 {
                            if graph.cfg.shared3_tab.len() != 16 { return Err(EINVAL); }
                            for (word, value) in policy[0x62b8..0x62f8].chunks_exact_mut(4).zip(graph.cfg.shared3_tab) {
                                word.copy_from_slice(&value.to_le_bytes());
                            }
                            let mask_at = |at: usize| -> Result<u64> {
                                Ok(u64::from_le_bytes(policy[at..at+8].try_into().map_err(|_| EINVAL)?))
                            };
                            dev_info!(graph.dev.as_ref(),
                                "G14G prepared thermal masks inventory={:#x} fast_die={:#x} soc_hot={:#x}\n",
                                mask_at(0x3ba0)?, mask_at(0x2278)?, mask_at(0x2f28)?);
                        }
                        // Controlled cold-wake operand, distinct from the observed current index.
                        if base_request {
                            if pwr.perf_base_pstate == 0 || pwr.perf_base_pstate as usize >= pwr.perf_states.len() {
                                return Err(EINVAL);
                            }
                            policy[0xa4..0xa8].copy_from_slice(&pwr.perf_base_pstate.to_le_bytes());
                        }
                        for (source, bits) in [
                            (4, pwr.pwr_sample_period_aic_clks),
                            (8, pwr.pwr_sample_period_aic_clks),
                            // The common 26-series controller has two initial state sources.
                            // The current state at +0x38 belongs to firmware.
                            (0x2c, pwr.perf_base_pstate),
                            (0x30, pwr.perf_base_pstate),
                            (0x9f0, pwr.pwr_min_duty_cycle),
                            (0xb5c, pwr.perf_integral_gain.to_bits()),
                            (0xb60, pwr.perf_integral_gain2.to_bits()),
                            (0xb6c, pwr.perf_proportional_gain.to_bits()),
                            (0xb70, pwr.perf_proportional_gain2.to_bits()),
                        ] {
                            let target = abi::relocate_by_bands(abi::COMMON26_POLICY_BANDS, source)
                                .ok_or(EINVAL)?;
                            policy.get_mut(target..target + 4).ok_or(EINVAL)?
                                .copy_from_slice(&bits.to_le_bytes());
                        }
                        if power_filter {
                            // The 26B123 controller uses the same calibrated values as J413's
                            // typed producer. Only the layout and the peak length unit differ.
                            let samples = pwr.ppm_filter_time_constant_ms / pwr.power_sample_period;
                            let fields = base.hwdata_a.with(|a, _| [
                                (0x50, 100), (0xa80, a.ppm_filter_tc_periods_x4),
                                (0xa8c, a.ppm_filter_a_neg.to_bits()), (0xa94, a.ppm_filter_a.to_bits()),
                                (0xa9c, a.ppm_ki_dt.to_bits()), (0xaa8, a.unk_6fc.to_bits()),
                                (0xaac, a.ppm_kp.to_bits()), (0xab8, a.pwr_min_duty_cycle),
                                (0xabc, a.max_pstate_scaled_4), (0xacc, a.max_power_3),
                                (0xad8, samples), (0x2200, a.unk_6fc.to_bits()),
                                (0xa48, max_state),
                                (0xb38, a.perf_tgt_utilization), (0xb40, a.perf_boost_min_util),
                                (0xb44, a.perf_boost_ce_step), (0xb58, a.perf_filter_drop_threshold),
                                (0xb5c, a.perf_filter_a_neg.to_bits()), (0xb60, a.perf_filter_a2_neg.to_bits()),
                                (0xb64, a.perf_filter_a.to_bits()), (0xb68, a.perf_filter_a2.to_bits()),
                                (0xb74, a.perf_integral_min_clamp.to_bits()), (0xb78, a.unk_79c.to_bits()),
                                (0xb84, a.boost_state_unk_k.to_bits()), (0xb88, a.base_pstate_scaled_2),
                                (0xb8c, a.max_pstate_scaled_5), (0xb90, a.base_pstate_scaled_3),
                                (0xb9c, a.perf_tgt_utilization_2), (0xbd0, a.base_pstate_scaled_4),
                                (0x2204, a.pwr_min_duty_cycle_2.to_bits()), (0x2208, a.max_pstate_scaled_6.to_bits()),
                                (0x220c, pwr.max_freq_mhz),
                                (0x22b0, a.unk_8a8.to_bits()), (0x22c0, a.pwr_min_duty_cycle_4),
                                (0x22c4, a.max_pstate_scaled_8), (0x22c8, a.max_pstate_scaled_9),
                                (0x2308, a.max_pstate_scaled_10),
                                (0x28e8, crate::module_parameters::t8112_power_zones.value()), (0x28f0, a.max_power_4),
                                (0x28f4, a.max_power_5), (0x28f8, a.max_power_6),
                                (0x2904, a.avg_power_target_filter_a_neg.to_bits()),
                                (0x2908, a.avg_power_target_filter_a.to_bits()),
                                (0x29f0, a.avg_power_filter_a_neg.to_bits()),
                                (0x29f8, a.avg_power_filter_a.to_bits()),
                                (0x2a00, a.avg_power_ki_dt.to_bits()), (0x2a10, a.avg_power_kp.to_bits()),
                                (0x2a0c, a.unk_d20.to_bits()), (0x2a1c, a.avg_power_min_duty_cycle),
                                (0x2a20, a.max_pstate_scaled_12), (0x2a2c, a.max_power_7.to_bits()),
                                (0x2a30, a.max_power_8),
                                (0x2b80, a.unk_e10_0.gpu_se_filter_a_neg.to_bits()),
                                (0x2b84, a.unk_e10_0.gpu_se_filter_1_a_neg.to_bits()),
                                (0x2b88, a.unk_e10_0.gpu_se_filter_a.to_bits()),
                                (0x2b8c, a.unk_e10_0.gpu_se_filter_1_a.to_bits()),
                                (0x2b90, a.unk_e10_0.gpu_se_ki_dt.to_bits()),
                                (0x2b94, a.unk_e10_0.gpu_se_ki_1_dt.to_bits()),
                                (0x2b9c, a.unk_e10_0.unk_7c.to_bits()),
                                (0x2ba0, a.unk_e10_0.gpu_se_kp.to_bits()),
                                (0x2ba4, a.unk_e10_0.gpu_se_kp_1.to_bits()),
                                (0x2bac, a.unk_e10_0.unk_8c), (0x2bb0, a.unk_e10_0.max_pstate_scaled_1),
                                (0x2c10, a.unk_e10_0.unk_3c), (0x2c14, a.unk_e10_0.unk_38),
                                (0x2c28, a.unk_e10_0.gpu_se_reset_criteria),
                                (0x6ac8, 1),
                            ]);
                            for (at, value) in fields {
                                policy[at..at+4].copy_from_slice(&value.to_le_bytes());
                            }
                            let ticks = u64::from(samples) * u64::from(pwr.pwr_sample_period_aic_clks);
                            policy[0xae0..0xae8].copy_from_slice(&ticks.to_le_bytes());
                            dev_info!(graph.dev.as_ref(),
                                "G14G cold power policy target={} peak samples={} ticks={} ceiling={}\n",
                                pwr.max_power_mw, samples, ticks, max_state);
                        }
                        crate::pgtable::clean_poc(policy.as_ptr(), policy.len());
                    }
                    let address = owner.gpu_va().get().to_le_bytes();
                    dev_info!(graph.dev.as_ref(), "G14G directory +{:#x} owner va={:#x} bytes={:#x}\n",
                              offset, owner.gpu_va().get(), size);
                    bytes[offset..offset+8].copy_from_slice(&address);
                    if offset == 0x10 {
                        bytes[8..16].copy_from_slice(&address);
                    }
                    if offset == (if crate::module_parameters::t8112_provider_object.value() == 1 { 0x26c } else { 0x264 }) {
                        if let Some(job) = startup_fence.as_mut() {
                            // Compute engine state: legacy StartCompute/FinalizeCompute "stats".
                            let patch: KVec<usize> = core::mem::take(&mut job.stats_patch);
                            job.compute_state = owner.gpu_va().get();
                            let stream = job._owners[if job.second.is_some() { 8 } else { 4 }].as_mut_slice();
                            for at in patch.iter().copied() {
                                stream[at..at + 8].copy_from_slice(&address);
                            }
                            crate::pgtable::clean_poc(stream.as_ptr(), stream.len());
                        }
                    }
                    if offset == 0x485 {
                        controller = Some(owner);
                    } else {
                        channels.push(owner, GFP_KERNEL)?;
                    }
                }
                let ids = alloc.private.array_empty::<u8>(0x134c)?;
                for (slot, offset) in [(0x2c8, 0), (0x2d0, 0x1000), (0x2d8, 0x1300)] {
                    bytes[slot..slot+8].copy_from_slice(&(ids.gpu_va().get() + offset).to_le_bytes());
                }
                channels.push(ids, GFP_KERNEL)?;
                let low = base.buffer_mgr_ctl_low_mapping.as_ref().ok_or(EINVAL)?;
                let high = base.buffer_mgr_ctl_high_mapping.as_ref().ok_or(EINVAL)?;
                if low.translate_offset(0)? != high.translate_offset(0)? { return Err(EINVAL); }
                bytes[0x2e0..0x2e8].copy_from_slice(&low.iova().to_le_bytes());
                bytes[0x2e8..0x2f0].copy_from_slice(&high.iova().to_le_bytes());
                bytes[0x3e4] = 0xff;
            }
            if matches!(header, G14DirectoryHeader::PrivatePage) {
                // T6040's main.unk_18 names a zeroed, private RAM page.
                // Keep a page-aligned allocation alive for the whole firmware session.
                let size = mmu::UAT_PGSZ;
                let storage = alloc.private.alloc_generic::<u8>(size, size, None)?;
                let page = fw::types::GpuArray::<u8>::empty(storage, size)?;
                if page.gpu_va().get() % size as u64 != 0 {
                    return Err(EINVAL);
                }
                crate::pgtable::clean_poc(page.as_slice().as_ptr(), size);
                bytes[0x18..0x20].copy_from_slice(&page.gpu_va().get().to_le_bytes());
                channels.push(page, GFP_KERNEL)?;
            }
            let payload = alloc.shared.array_empty::<u8>(
                extended::LOG_THREADS * 256 * extended::LOG_PAYLOAD_BYTES)?;
            bytes[0x200..0x208].copy_from_slice(&payload.gpu_va().get().to_le_bytes());
            channels.push(payload, GFP_KERNEL)?;
            if board && crate::module_parameters::t8112_firmware_log.value() {
                // J613's extended runtime opens this gate; stock J413 leaves it clear.
                bytes[0x250..0x254].copy_from_slice(&1u32.to_le_bytes());
            }
            crate::pgtable::clean_poc(bytes.as_ptr(), bytes.len());
            Some(directory)
        } else {
            None
        };
        let mut status = alloc.shared.array_empty::<u8>(0x20)?;
        if board && crate::module_parameters::t8112_firmware_trace.value() {
            // Debug bit 1 in the status word, set before the root message.
            status.as_mut_slice()[..4].copy_from_slice(&crate::module_parameters::t8112_trace_bits.value().to_le_bytes());
        }
        let boot_page = if board {
            let page = alloc.shared_ro.alloc_generic::<u8>(mmu::UAT_PGSZ, mmu::UAT_PGSZ, None)?;
            Some(fw::types::GpuArray::empty(page, mmu::UAT_PGSZ)?)
        } else {
            None
        };
        let globals = if board {
            let mut globals = alloc.shared_ro.array_empty::<u8>(0xe48)?;
            let data = globals.as_mut_slice();
            for (offset, value) in [(0x58, 0xffffu16), (0x5a, 0x28), (0x5c, 0xffff)] {
                data[offset..offset+2].copy_from_slice(&value.to_le_bytes());
            }
            if power_filter {
                let ceiling = pwr.perf_max_pstate * 100;
                // An upper cap of 100 at +0x98 keeps the first job at the
                // base state, with no operating-point change around submission.
                let cap = match crate::module_parameters::t8112_state_cap.value() { 0 => ceiling, cap => cap };
                for (at, value) in [(0x84, pwr.max_power_mw), (0x88, ceiling),
                                    (0x8c, ceiling), (0x98, cap)] {
                    data[at..at+4].copy_from_slice(&value.to_le_bytes());
                }
            }
            // Hardware configuration +0xff0 is zero in this profile.
            data[0x6e..0x72].copy_from_slice(&1u32.to_le_bytes());
            crate::pgtable::clean_poc(data.as_ptr(), data.len());
            Some(globals)
        } else {
            None
        };
        let mut control = alloc.shared.array_empty(if board { 0xe850 } else { 0xeaf0 })?;
        if board {
            // One shared backing: 0x20 bytes of indices, followed at +0x40
            // by 256 cache-flush requests. An empty ring still needs both VAs.
            let flush = alloc.shared.array_empty::<u8>(0x40 + 256 * 0x18)?;
            let state = flush.gpu_va().get();
            extended::write_flush_ring(control.as_mut_slice(), 0x4be0, state, state + 0x40)
                .map_err(|_| EINVAL)?;
            if crate::module_parameters::t8112_command_service.value() {
                // Request command-handler installation during powered initialization.
                control.as_mut_slice()[0x4c24..0x4c28].copy_from_slice(&1u32.to_le_bytes());
            }
            channels.push(flush, GFP_KERNEL)?;
            dev_info!(graph.dev.as_ref(), "G14G 26B123 shared temperature and cache-flush allocations ready; GPTBAT={:#x}\n",
                      graph.dyncfg.uat_ttb_base);
        }
        let mut power = alloc.shared.array_empty(if board { 0x6a0 } else if directory_layout.is_some() { 0x698 } else { 0x670 })?;
        if power_filter {
            // Cold mirrors only. A runtime update must also publish its filter length.
            let gains = graph.initdata.runtime_pointers.hwdata_a.with(|a, _|
                (a.ppm_kp.to_bits(), a.ppm_ki_dt.to_bits()));
            let data = power.as_mut_slice();
            for (at, value) in [(0x12c, pwr.max_power_mw), (0x130, gains.0), (0x134, gains.1)] {
                data[at..at+4].copy_from_slice(&value.to_le_bytes());
            }
            // The SGX cold runtime mirrors use the same temperature units and gains
            // as the controller. Leave its update flag and overrides at their cold values.
            let initial = controller.as_ref().ok_or(EINVAL)?.as_slice();
            for (dst, src) in [(0xc8, 0x22d4), (0xcc, 0x2280), (0xd0, 0x2284),
                               (0xd4, 0x22cc), (0xd8, 0x22b4), (0xdc, 0x22a4)] {
                data[dst..dst+4].copy_from_slice(&initial[src..src+4]);
            }
            crate::pgtable::clean_poc(data.as_ptr(), data.len());
        }
        let extra = match board_extra {
            Some(extra) => extra,
            None => alloc.shared.array_empty(0x4000)?,
        };
        let (page_size, page_bits, levels, descriptors) = graph.initdata.with(|r, _|
            (r.uat_page_size, r.uat_page_bits, r.uat_num_levels, r.uat_level_info));
        let root = if board {
            alloc.shared_ro.alloc_generic::<Self>(mmu::UAT_PGSZ, mmu::UAT_PGSZ, None)?
        } else {
            alloc.private.alloc_object::<Self>()?
        };
        fw::types::GpuObject::<Self>::new_init_prealloc(root,
            |_| try_init!(Self { graph: graph.clone(), directory, controller, slot_ram, _channels: channels, _register_mappings: register_mappings, startup_fence, globals, boot_page, status, control, power, extra }),
            |owners, _| {
                let base = &owners.graph.initdata;
                try_init!(fw::initdata::raw::InitDataG14V27_2 {
                    ver_info: version_words(word),
                    unk_buf: owners.boot_page.as_ref().unwrap_or(&base.unk_buf).gpu_pointer(),
                    unk_8: 0,
                    unk_c: 0,
                    runtime_pointers: match &owners.directory {
                        Some(directory) => directory.gpu_pointer(),
                        None => base.runtime_pointers.gpu_bytes(),
                    },
                    globals: match &owners.globals { Some(g) => g.gpu_pointer(), None => base.globals.gpu_bytes() },
                    role: 0,
                    host_mapped_fw_allocations: 1,
                    uat_page_size: page_size,
                    uat_page_bits: page_bits,
                    uat_num_levels: levels,
                    uat_level_info: descriptors,
                    __pad0: Default::default(),
                    status_record: owners.status.gpu_pointer(),
                    control: owners.control.gpu_pointer(),
                    power: owners.power.gpu_pointer(),
                    extra: owners.extra.gpu_pointer(),
                })
            },
        )
    }
}
