// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The G14 root for GPU firmware 27.2, the boot compute job that starts the firmware's command
//! service, and the command pool the 27.2 compute runtime (`crate::g14_v27_2`) publishes from.
//!
//! The root is built on the records of a 13.5 G14 manager, made with
//! [`GraphProfile::G14V27_2`]: its hardware data supplies the calibrated values, and its
//! kernel UAT, allocators and buffer-manager control pages are shared. The firmware never sees
//! that manager's own root.

use super::*;
use kernel::device;
use crate::g14_v27_2_abi::{self, EventKind, GrowRequest, QueueError};
use crate::g17::fw::m5::initdata as abi;
use crate::hw::{RegisterBaseV27_2, RegisterV27_2};
use crate::m3::m3_init_26 as directory26;

/// The firmware build word this root carries in its version field.
const FIRMWARE_WORD: u64 = 0x0c8a_8342_8392_04c8;

/// Kick message for the compute submission ring the root uses (group 2, compute).
pub(crate) const COMPUTE_KICK: u64 = MSG_TX_DOORBELL | (4 * PRIORITY_GROUP + COMPUTE) as u64;
/// Kick message for the device-control ring.
pub(crate) const DEVICE_CONTROL_KICK: u64 = MSG_FWCTL | DOORBELL_DEVCTRL;

/// Data master of the root's queue, and its priority group: the legacy G14 priority row 2.
const COMPUTE: usize = 2;
const PRIORITY_GROUP: usize = 2;
const PRIORITY_ROW: usize = 2;
/// The submission descriptor the queue publishes on (three per group).
const RENDER_GROUP: usize = 0;
pub(crate) const TILER: usize = 0;
pub(crate) const FRAGMENT: usize = 1;
const SUBMISSION_RING: usize = 3 * PRIORITY_GROUP + COMPUTE;
/// Firmware event, log, trace and statistics rings in the 34-entry channel list.
const EVENT_RING: usize = 13;
const LOG_RINGS: usize = 14;
const TRACE_RING: usize = 15;
const STATISTICS_RING: usize = 16;
/// Ring geometries: the trace ring has 512 records of 0x48 bytes, the others 256.
const TRACE_RECORDS: u32 = 512;
const RING_RECORDS: u32 = 256;
use crate::g14_v27_2_abi::{
    RECOVERY_GENERATION as CONTROL_RECOVERY_GENERATION, RECOVERY_PHASE as CONTROL_RECOVERY_PHASE,
    RECOVERY_SNAPSHOT as CONTROL_RECOVERY_SNAPSHOT,
};
pub(crate) use crate::g14_v27_2_abi::{stopped_tiler, RECOVERY_SNAPSHOT_ENTRIES};
/// Queue capacity at position block +0x50 (16 x the 0x50 channels).
const QUEUE_CAPACITY: u32 = 0x500;
/// Globals offsets of the 13.5 shared power tables (HwDataShared1 and HwDataShared2): the 27.2
/// Globals carries them at these places, laid out field for field as in the 13.5 InitData.
const GLOBALS_SHARED1: usize = 0xec;
const GLOBALS_SHARED2: usize = 0x1b4;
const SHARED1_BYTES: usize = 0xa8;
const SHARED2_BYTES: usize = 0x510;
/// Offsets of the 13.5 HwDataShared3 in the 27.2 power and controller records.
const POWER_SHARED3: usize = 0x4f0;
const CONTROLLER_SHARED3: usize = 0x62ac;
const CONTROLLER_BYTES: usize = 0x6c48;
const _: () = {
    assert!(core::mem::size_of::<fw::initdata::raw::HwDataShared1>() == SHARED1_BYTES);
    assert!(core::mem::size_of::<fw::initdata::raw::HwDataShared2>() == SHARED2_BYTES);
    assert!(GLOBALS_SHARED1 + SHARED1_BYTES <= GLOBALS_SHARED2);
    assert!(GLOBALS_SHARED2 + SHARED2_BYTES <= g14_v27_2_abi::GLOBALS_BYTES);
    // The offsets put_shared1/put_shared2/put_curve write.
    use core::mem::offset_of;
    use fw::initdata::raw::{HwDataShared1 as S1, HwDataShared2 as S2, HwDataShared2Curve as C,
                            HwDataShared2G14 as G};
    assert!(offset_of!(S1, unk_44) == 0x40 && offset_of!(S1, unk_a4) == 0xa0 && offset_of!(S1, unk_a8) == 0xa4);
    assert!(offset_of!(S2, unk_28) == 0x28 && offset_of!(S2, g14) == 0x38 && offset_of!(S2, unk_500) == 0x500);
    assert!(offset_of!(G, unk_14) == 0x14 && offset_of!(G, unk_18) == 0x18);
    assert!(offset_of!(G, curve1) == 0x38 && offset_of!(G, curve2) == 0x280);
    assert!(offset_of!(C, t1) == 8 && offset_of!(C, t2) == 0x28 && offset_of!(C, t3) == 0x48);
    assert!(core::mem::size_of::<C>() == 0x248);
    // The 13.5 HwDataB words copied at the chip id's distance land where the 27.2 block has
    // them (power sample period +0xff4, fragment count +0x10a4).
    use fw::initdata::raw::{HwDataBG14V13_5 as B13, HwDataBG14V27_2 as B27};
    assert!(offset_of!(B13, power_sample_period) + offset_of!(B27, chip_id) - offset_of!(B13, chip_id) == 0xff4);
    assert!(offset_of!(B13, num_frags) + offset_of!(B27, chip_id) - offset_of!(B13, chip_id) == 0x10a4);
    use fw::initdata::raw::HwDataShared3 as S3;
    assert!(core::mem::size_of::<S3>() == 0x50 && offset_of!(S3, table) == 0xc && offset_of!(S3, unk_4c) == 0x4c);
    assert!(POWER_SHARED3 + core::mem::size_of::<S3>() <= 0x6a0);
    assert!(CONTROLLER_SHARED3 + offset_of!(S3, table) == 0x62b8);
    assert!(CONTROLLER_SHARED3 + core::mem::size_of::<S3>() <= CONTROLLER_BYTES);
};

/// Board facts the root needs, read from the device tree at probe.
pub(crate) struct BoardV27_2 {
    /// The SGX register window (`reg` "sgx").
    pub(crate) sgx: u64,
    pub(crate) sgx_size: u64,
    /// Bytes of the firmware's crash copy buffer (`apple,coredump-copy-buffer`).
    pub(crate) copy_bytes: u32,
    /// The chip's 27.2 register table.
    pub(crate) registers: &'static [RegisterV27_2],
    /// Slots whose stock mode is read-write (bit per slot).
    pub(crate) writable: u64,
}

/// Firmware-visible facts of the root's queue that every compute command names.
#[derive(Copy, Clone)]
pub(crate) struct QueueInfo {
    pub(crate) queue: u64,
    pub(crate) slot: u32,
    pub(crate) stamp: u64,
    pub(crate) fw_stamp: u64,
    pub(crate) compute_state: u64,
}

/// One compute command, as the 26.x submission front end hands it over.
pub(crate) struct ComputeJob {
    pub(crate) context: u32,
    pub(crate) sequence: u64,
    pub(crate) cdm_base: u64,
    pub(crate) cdm_end: u64,
    pub(crate) usc_base: u64,
    /// VA of the preemption buffer in the same context (13.5 layout: buffer 1 at +0,
    /// buffers 2 to 5 at +compute_preempt1_size + 8n).
    pub(crate) preempt: u64,
    /// Firmware VAs of the user start and end timestamps, or 0.
    pub(crate) timestamps: [u64; 2],
    /// The helper program (scratch and stack setup), or zero.
    pub(crate) helper: crate::m3::agx_uapi::UapiHelperProgram,
    /// The sampler heap and its sampler count, or zero.
    pub(crate) sampler_heap: u64,
    pub(crate) sampler_count: u32,
}

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

/// The 13.5 HwDataShared1, field for field.
fn put_shared1(b: &mut [u8], at: usize, s: &fw::initdata::raw::HwDataShared1) {
    for (i, v) in s.table.iter().enumerate() {
        put32(b, at + 4 * i, *v as u32);
    }
    b[at + 0x40..at + 0xa0].copy_from_slice(&(*s.unk_44)[..]);
    put32(b, at + 0xa0, s.unk_a4);
    put32(b, at + 0xa4, s.unk_a8);
}

/// The 13.5 HwDataShared2 curve, field for field.
fn put_curve(b: &mut [u8], at: usize, c: &fw::initdata::raw::HwDataShared2Curve) {
    put32(b, at, c.unk_0);
    put32(b, at + 4, c.unk_4);
    for (i, v) in c.t1.iter().enumerate() {
        b[at + 8 + 2 * i..at + 10 + 2 * i].copy_from_slice(&v.to_le_bytes());
    }
    for (i, v) in c.t2.iter().enumerate() {
        b[at + 0x28 + 2 * i..at + 0x2a + 2 * i].copy_from_slice(&v.to_le_bytes());
    }
    for (row, values) in c.t3.iter().enumerate() {
        for (i, v) in values.iter().enumerate() {
            put32(b, at + 0x48 + 0x40 * row + 4 * i, *v as u32);
        }
    }
}

/// The 13.5 HwDataShared3, field for field.
fn put_shared3(b: &mut [u8], at: usize, s: &fw::initdata::raw::HwDataShared3) {
    for (i, v) in [s.unk_0, s.unk_4, s.unk_8].into_iter().enumerate() {
        put32(b, at + 4 * i, v);
    }
    for (i, v) in s.table.iter().enumerate() {
        put32(b, at + 0xc + 4 * i, *v);
    }
    put32(b, at + 0x4c, s.unk_4c);
}

/// The 13.5 HwDataShared2, field for field (G14 curves included).
fn put_shared2(b: &mut [u8], at: usize, s: &fw::initdata::raw::HwDataShared2) {
    for (i, v) in s.table.iter().enumerate() {
        put32(b, at + 4 * i, *v as u32);
    }
    b[at + 0x28..at + 0x38].copy_from_slice(&(*s.unk_28)[..]);
    for (i, v) in s.g14.unk_0.iter().enumerate() {
        put32(b, at + 0x38 + 4 * i, *v);
    }
    put32(b, at + 0x4c, s.g14.unk_14);
    for (i, v) in s.g14.unk_18.iter().enumerate() {
        put32(b, at + 0x50 + 4 * i, *v);
    }
    put_curve(b, at + 0x70, &s.g14.curve1);
    put_curve(b, at + 0x2b8, &s.g14.curve2);
    for (i, v) in [s.unk_500, s.unk_504, s.unk_508, s.unk_50c].into_iter().enumerate() {
        put32(b, at + 0x500 + 4 * i, v);
    }
}

fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

/// Encodes a type-3 compute command (0x358 bytes, the 13.5 G14 job parameters inside the
/// 27.2 wrapper) and its start/finish stream. Both buffers must be zeroed.
#[allow(clippy::too_many_arguments)]
fn encode_compute(
    cmd: &mut [u8], cmd_va: u64, stream: &mut [u8], stream_va: u64, tracking: u64,
    timestamps: u64, preempt1: u64, q: &QueueInfo, stamp_value: u32, job: &ComputeJob,
) {
    put32(cmd, 0, 3);
    put64(cmd, 4, job.sequence);
    put32(cmd, 0xc, job.context);
    put64(cmd, 0x10, tracking);
    // 13.5 JobParameters1 at +0x68.
    let p2 = job.preempt + preempt1;
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
    // flush_stamps, as the render commands and the M3 27.2 compute command set it.
    put32(cmd, g14_v27_2_abi::COMPUTE_FLUSH_STAMPS, 1);
    put64(cmd, 0x2cc, timestamps);
    put64(cmd, 0x2d4, timestamps + 8);
    // Command-local dependency latch, enabled with seed 0; the firmware clears 0x10 bytes
    // through the pointer at +0x334.
    put32(cmd, 0x33c, 1);
    put64(cmd, 0x334, timestamps + 0x20);
    let user = g14_v27_2_abi::encode_compute_timestamps(cmd, cmd_va, job.timestamps);
    g14_v27_2_abi::encode_compute_helper(cmd, job.helper.binary, job.helper.data, job.helper.config);
    g14_v27_2_abi::encode_compute_samplers(cmd, job.sampler_heap, job.sampler_count);

    put32(stream, 0, 11);
    // 13.5 StartCompute.unk_28, at the same place in this layout (the 13.5 G14 path writes 1).
    put32(stream, 0x28, 1);
    put64(stream, 0x160, job.sequence);
    put64(stream, 0x04, cmd_va + 0x18);
    put64(stream, 0x0c, cmd_va + 0x68);
    put64(stream, 0x14, q.compute_state);
    put64(stream, 0x1c, q.queue);
    put32(stream, 0x24, job.context);
    put32(stream, 0x34, q.slot);
    put64(stream, 0x3c, cmd_va + 0x1f4);
    put64(stream, 0x158, cmd_va + 0x340);
    for (at, op, selected) in [(0x16c, 0x8000_0003u32, 0x2ccu64), (0x1bc, 3, 0x2d4)] {
        put32(stream, at, op);
        put64(stream, at + 0x04, cmd_va + 0x2c4);
        put64(stream, at + 0x0c, cmd_va + 0x2cc);
        put64(stream, at + 0x14, cmd_va + selected);
        put64(stream, at + 0x1c, q.queue);
        put64(stream, at + 0x24, user);
        put64(stream, at + 0x2c, cmd_va + 0x314);
        put64(stream, at + 0x34, cmd_va + 0x264);
    }
    put32(stream, 0x1b8, 0x0080_0000);
    let finish = 0x208;
    put32(stream, finish, 12);
    put64(stream, finish + 4, q.compute_state);
    put64(stream, finish + 0x0c, q.queue);
    put32(stream, finish + 0x14, job.context);
    put64(stream, finish + 0x18, cmd_va + 0x1f4);
    put64(stream, finish + 0x2c, q.fw_stamp);
    put32(stream, finish + 0x34, stamp_value);
    put32(stream, finish + 0x5c, (-(finish as i32)) as u32);
    put64(stream, finish + 0x71, cmd_va + 0x340);
    put64(stream, finish + 0x79, cmd_va + 0x304);
    put32(stream, finish + 0x84, 0x4000_0002);
}

/// A persistent firmware queue with a separate scheduler context and stamp slot per data master.
struct DmQueue {
    queue: fw::types::GpuArray<u8>,
    positions: fw::types::GpuArray<u8>,
    pointers: fw::types::GpuArray<u8>,
    event: event::Event,
    announced: core::sync::atomic::AtomicBool,
    _scheduler: fw::types::GpuArray<u8>,
    _stamps: fw::types::GpuArray<u8>,
}

impl DmQueue {
    fn new(alloc: &mut KernelAllocators, event: event::Event, priority: usize) -> Result<Self> {
        let mut queue = alloc.private.array_empty::<u8>(0x2cc0)?;
        let mut positions = alloc.shared.array_empty::<u8>(0x2860)?;
        let pointers = alloc.private.array_empty::<u8>(0x2860)?;
        let mut scheduler = alloc.private.array_empty::<u8>(0x40)?;
        let mut queue_stamps = alloc.shared.array_empty::<u8>(0x18)?;
        let queue_stamps_va = queue_stamps.gpu_va().get();
        put64(queue_stamps.as_mut_slice(), 8, queue_stamps_va);
        let ctx = scheduler.as_mut_slice();
        ctx[..2].copy_from_slice(&u16::MAX.to_le_bytes());
        ctx[5] = 1;
        ctx[0x27] = 0xff;
        let queue_va = queue.gpu_va().get();
        let q = queue.as_mut_slice();
        for (offset, value) in [(0, positions.gpu_va().get()), (8, pointers.gpu_va().get()),
                                (0x10, queue_stamps_va), (0x18, queue_va + 0x90),
                                (0x7c, scheduler.gpu_va().get())] {
            put64(q, offset, value);
        }
        // The kernel-owned queue has no owning process.
        for offset in [0x2c, 0x4c, 0x50] {
            put32(q, offset, u32::MAX);
        }
        let row = crate::fw::workqueue::raw::PRIORITY[priority];
        put32(q, 0x30, row.0);
        put32(q, 0x34, row.1);
        put64(q, 0x38, row.2.0);
        put32(q, 0x40, row.3);
        put32(q, 0x44, row.4);
        // The current lane stays at the all-ones sentinel; the firmware assigns it.
        put32(q, 0x48, u32::MAX);
        crate::pgtable::clean_poc(q.as_ptr(), q.len());
        put32(positions.as_mut_slice(), 0x50, QUEUE_CAPACITY);
        for mapping in [&positions, &pointers, &scheduler, &queue_stamps] {
            crate::pgtable::clean_poc(mapping.as_slice().as_ptr(), mapping.as_slice().len());
        }
        Ok(Self { queue, positions, pointers, event,
            announced: core::sync::atomic::AtomicBool::new(false),
            _scheduler: scheduler, _stamps: queue_stamps })
    }

    fn info(&self, engine_state: u64) -> QueueInfo {
        QueueInfo {
            queue: self.queue.gpu_va().get(),
            slot: self.event.slot(),
            stamp: u64::from(self.event.stamp_pointer()),
            fw_stamp: u64::from(self.event.fw_stamp_pointer()),
            compute_state: engine_state,
        }
    }
}

/// A prepared submission has written only unowned ring entries. Publishing its cursors is
/// infallible, so a pair can be staged before either of its queues becomes firmware-visible.
pub(crate) struct Submission<'a> {
    head: &'a core::sync::atomic::AtomicU32,
    positions: &'a [u8],
    target: u32,
    producer: &'a core::sync::atomic::AtomicU32,
    indices: &'a [u8],
    next: u32,
    announced: Option<&'a core::sync::atomic::AtomicBool>,
}

impl Submission<'_> {
    pub(crate) fn target(&self) -> u32 { self.target }

    pub(crate) fn publish(self) {
        self.head.store(self.target, Ordering::Release);
        crate::pgtable::clean_poc(self.positions.as_ptr(), 0x60);
        crate::pgtable::poc_barrier();
        self.producer.store(self.next, Ordering::Release);
        crate::pgtable::clean_poc(self.indices.as_ptr(), 0x30);
        crate::pgtable::poc_barrier();
        if let Some(announced) = self.announced {
            announced.store(true, Ordering::Release);
        }
    }
}

/// The kernel-owned boot dispatch must retire before clients use the compute queue.
struct BootJob {
    queue: fw::types::GpuArray<u8>,
    positions: fw::types::GpuArray<u8>,
    pointers: fw::types::GpuArray<u8>,
    command: fw::types::GpuArray<u8>,
    stream: fw::types::GpuArray<u8>,
    tracking: fw::types::GpuArray<u8>,
    timestamps: fw::types::GpuArray<u8>,
    event: event::Event,
    expected_stamp: u32,
    _owners: KVec<fw::types::GpuArray<u8>>,
    /// The job's buffers in context 0: mapping before backing.
    _client: (mmu::KernelMapping, gem::ObjectRef),
    client_va: u64,
    preempt1: u64,
}

impl BootJob {
    fn new(alloc: &mut KernelAllocators, event: event::Event, uat: &mmu::Uat, dev: &AsahiDevice,
           cfg: &'static hw::HwConfig) -> Result<Self> {
        let mut queue = alloc.private.array_empty::<u8>(0x2cc0)?;
        let mut positions = alloc.shared.array_empty::<u8>(0x2860)?;
        let pointers = alloc.private.array_empty::<u8>(0x2860)?;
        let mut scheduler = alloc.private.array_empty::<u8>(0x40)?;
        let mut queue_stamps = alloc.shared.array_empty::<u8>(0x18)?;
        let command = alloc.private.array_empty::<u8>(0x400)?;
        let stream = alloc.private.array_empty::<u8>(0x400)?;
        // A command-buffer tracking slot (0x100 stride, 0xc8 bytes kept by the firmware)
        // whose first word names its own cleared retirement cursor.
        let mut tracking = alloc.private.array_empty::<u8>(0x100)?;
        let cursor = alloc.shared.array_empty::<u8>(0x90)?;
        put64(tracking.as_mut_slice(), 0, cursor.gpu_va().get());
        crate::pgtable::clean_poc(tracking.as_slice().as_ptr(), 0x100);

        let queue_stamps_va = queue_stamps.gpu_va().get();
        put64(queue_stamps.as_mut_slice(), 8, queue_stamps_va);
        let ctx = scheduler.as_mut_slice();
        ctx[..2].copy_from_slice(&u16::MAX.to_le_bytes());
        ctx[5] = 1;
        ctx[0x27] = 0xff;
        let queue_va = queue.gpu_va().get();
        let q = queue.as_mut_slice();
        for (offset, value) in [(0, positions.gpu_va().get()), (8, pointers.gpu_va().get()),
                                (0x10, queue_stamps_va), (0x18, queue_va + 0x90),
                                (0x7c, scheduler.gpu_va().get())] {
            put64(q, offset, value);
        }
        // The kernel-owned queue has no owning process.
        for offset in [0x2c, 0x4c, 0x50] {
            put32(q, offset, u32::MAX);
        }
        let row = crate::fw::workqueue::raw::PRIORITY[PRIORITY_ROW];
        put32(q, 0x30, row.0);
        put32(q, 0x34, row.1);
        put64(q, 0x38, row.2.0);
        put32(q, 0x40, row.3);
        put32(q, 0x44, row.4);
        // The current lane stays at the all-ones sentinel; the firmware assigns it.
        put32(q, 0x48, u32::MAX);
        crate::pgtable::clean_poc(q.as_ptr(), q.len());
        put32(positions.as_mut_slice(), 0x50, QUEUE_CAPACITY);

        // The job's preemption buffer and control stream live in context 0 at a client VA.
        let preempt1 = cfg.compute_preempt1_size;
        let client_va = IOVA_KERN_GPU_BUFMGR_LOW + 0x10_0000;
        let mut object = gem::new_kernel_object(dev, preempt1 + 0x4000)?;
        let size = object.size();
        {
            use kernel::io::Io;
            let map = object.vmap()?;
            for off in (0..size).step_by(8) { map.try_write64(0, off)?; }
            // CDM control stream: one Stream Terminate block (block type 2 in bits 31:29).
            map.try_write32(0x4000_0000, preempt1 + 0x2000)?;
        }
        let mapping = object.map_at(uat.kernel_lower_vm(), client_va, mmu::PROT_GPU_SHARED_RW, false)?;
        let timestamps = alloc.shared.array_empty::<u8>(0x80)?;

        let mut owners = KVec::new();
        for owner in [&queue_stamps, &pointers, &scheduler] {
            crate::pgtable::clean_poc(owner.as_slice().as_ptr(), owner.as_slice().len());
        }
        for owner in [queue_stamps, scheduler, cursor] {
            owners.push(owner, GFP_KERNEL)?;
        }
        let expected_stamp = event.current_noncoherent().next().raw();
        Ok(Self { queue, positions, pointers, command, stream, tracking, timestamps, event,
                  expected_stamp, _owners: owners, _client: (mapping, object),
                  client_va, preempt1: preempt1 as u64 })
    }

    fn info(&self, compute_state: u64) -> QueueInfo {
        QueueInfo {
            queue: self.queue.gpu_va().get(),
            slot: self.event.slot(),
            stamp: u64::from(self.event.stamp_pointer()),
            fw_stamp: u64::from(self.event.fw_stamp_pointer()),
            compute_state,
        }
    }

    /// Encodes the boot command and names it at queue position 0.
    fn encode(&mut self, q: &QueueInfo) {
        let cdm = self.client_va + self.preempt1 + 0x2000;
        let job = ComputeJob {
            context: 0, sequence: 0, cdm_base: cdm, cdm_end: cdm + 8,
            usc_base: 0x11_0000_0000, preempt: self.client_va, timestamps: [0; 2],
            helper: crate::m3::agx_uapi::UapiHelperProgram { binary: 0, config: 0, data: 0 },
            sampler_heap: 0, sampler_count: 0,
        };
        let (cmd_va, stream_va) = (self.command.gpu_va().get(), self.stream.gpu_va().get());
        encode_compute(self.command.as_mut_slice(), cmd_va, self.stream.as_mut_slice(), stream_va,
                       self.tracking.gpu_va().get(), self.timestamps.gpu_va().get(), self.preempt1,
                       q, self.expected_stamp, &job);
        crate::pgtable::clean_poc(self.stream.as_slice().as_ptr(), self.stream.as_slice().len());
        crate::pgtable::clean_poc(self.command.as_slice().as_ptr(), self.command.as_slice().len());
        put64(self.pointers.as_mut_slice(), 0, cmd_va);
        crate::pgtable::clean_poc(self.pointers.as_slice().as_ptr(), 8);
        crate::pgtable::poc_barrier();
    }
}

/// Command slots for client compute work. They are allocated with the root, before the
/// firmware window's tables are mirrored, so every slot lies in a table the firmware already
/// translates. Slots are reused in order.
pub(crate) const POOL_SLOTS: usize = 48;

const COMMAND_STRIDE: usize = 0x1000;
const STREAM_STRIDE: usize = 0x400;

pub(crate) struct CommandPool {
    /// Two command buffers per slot, used in turn (`g14_v27_2_abi::pool_command`).
    commands: fw::types::GpuArray<u8>,
    /// Which of its two command buffers each slot last published, one bit per slot.
    published: u64,
    streams: fw::types::GpuArray<u8>,
    tracking: fw::types::GpuArray<u8>,
    cursors: fw::types::GpuArray<u8>,
    stamps: fw::types::GpuArray<u8>,
    preempt1: u64,
}

impl CommandPool {
    pub(crate) fn new(alloc: &mut KernelAllocators, cfg: &'static hw::HwConfig) -> Result<Self> {
        let commands = alloc.private.array_empty::<u8>(2 * POOL_SLOTS * COMMAND_STRIDE)?;
        let streams = alloc.private.array_empty::<u8>(POOL_SLOTS * STREAM_STRIDE)?;
        let mut tracking = alloc.private.array_empty::<u8>(POOL_SLOTS * 0x100)?;
        // One 64-byte line per retirement cursor, so no firmware write can reach a neighbour.
        let cursors = alloc.shared.array_empty::<u8>(POOL_SLOTS * 0x40)?;
        let stamps = alloc.shared.array_empty::<u8>(POOL_SLOTS * 0x80)?;
        let base = cursors.gpu_va().get();
        for (i, chunk) in tracking.as_mut_slice().chunks_mut(0x100).enumerate() {
            put64(chunk, 0, base + 0x40 * i as u64);
        }
        crate::pgtable::clean_poc(tracking.as_slice().as_ptr(), tracking.as_slice().len());
        Ok(Self { commands, published: 0, streams, tracking, cursors, stamps,
                  preempt1: cfg.compute_preempt1_size as u64 })
    }

    /// Builds slot `i` for `job` and returns the command's firmware VA.
    pub(crate) fn encode(&mut self, i: usize, q: &QueueInfo, stamp_value: u32, job: &ComputeJob) -> Result<u64> {
        let preempt1 = self.preempt1;
        self.encode_with(i, job.context, job.sequence, stamp_value, |cmd, stream, common| {
            encode_compute(cmd, common.cmd_va, stream, common.stream_va, common.tracking,
                           common.timestamps, preempt1, q, stamp_value, job);
            Ok(())
        })
    }

    pub(crate) fn encode_with(
        &mut self, i: usize, context: u32, sequence: u64, stamp_value: u32,
        encode: impl FnOnce(&mut [u8], &mut [u8], crate::g14_v27_2_render::Common) -> Result,
    ) -> Result<u64> {
        if i >= POOL_SLOTS { return Err(EINVAL); }
        // The firmware may read a retired command's JobMeta after it has unlinked the slot
        // (for the stamp stores it still has queued), so the slot's next command goes to its
        // other buffer. A buffer is rewritten only after the slot has been retired and
        // unlinked once more, which the firmware does in a later pass. Encoding always
        // takes the spare; `published` moves it only once the firmware can see the command,
        // so a publication that fails leaves the last published buffer alone.
        let c = g14_v27_2_abi::pool_command(i, g14_v27_2_abi::spare_buffer(self.published, i));
        let cmd_va = self.commands.gpu_va().get() + (c * COMMAND_STRIDE) as u64;
        let stream_va = self.streams.gpu_va().get() + (i * STREAM_STRIDE) as u64;
        let tracking_va = self.tracking.gpu_va().get() + (i * 0x100) as u64;
        let timestamps_va = self.stamps.gpu_va().get() + (i * 0x80) as u64;
        let tracking = &mut self.tracking.as_mut_slice()[i * 0x100..(i + 1) * 0x100];
        tracking[8..].fill(0);
        crate::pgtable::clean_poc(tracking.as_ptr(), tracking.len());
        let stamps = &mut self.stamps.as_mut_slice()[i * 0x80..(i + 1) * 0x80];
        stamps.fill(0);
        crate::pgtable::clean_poc(stamps.as_ptr(), stamps.len());
        let cmd = &mut self.commands.as_mut_slice()[c * COMMAND_STRIDE..(c + 1) * COMMAND_STRIDE];
        cmd.fill(0);
        let stream = &mut self.streams.as_mut_slice()[i * STREAM_STRIDE..(i + 1) * STREAM_STRIDE];
        stream.fill(0);
        encode(cmd, stream, crate::g14_v27_2_render::Common {
            cmd_va, stream_va, tracking: tracking_va, timestamps: timestamps_va,
            context, sequence, stamp_value,
        })?;
        crate::pgtable::clean_poc(stream.as_ptr(), stream.len());
        crate::pgtable::clean_poc(cmd.as_ptr(), cmd.len());
        crate::pgtable::poc_barrier();
        Ok(cmd_va)
    }

    /// Slot `i`'s command, encoded in its spare buffer, is now visible to the firmware: the
    /// next encode of the slot takes the other buffer.
    pub(crate) fn mark_published(&mut self, i: usize) {
        if i < POOL_SLOTS { self.published ^= 1 << i; }
    }

    /// Retirement: advance slot `i`'s cursor to the value the firmware recorded at
    /// tracking +0xc, so its retirement scan unlinks the slot from the queue's list.
    ///
    /// The firmware records +0xc in its post-processing, which can run after the queue's
    /// retired position has advanced. Until then +0xc reads 0 (the slot is cleared for each
    /// use) and nothing is written: the cursor would stay behind and the firmware would keep
    /// the slot linked for good. Returns whether the cursor was advanced; the caller retries a
    /// slot that returned `false` on the firmware's next notification.
    pub(crate) fn retire(&self, i: usize) -> Result<bool> {
        if i >= POOL_SLOTS { return Err(EINVAL); }
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[i * 0x100..].as_ptr(), 0x100);
        crate::pgtable::poc_barrier();
        let expected = self.tracking.shared_index(i * 0x100 + 0xc)?.load(Ordering::Acquire);
        if expected == 0 { return Ok(false); }
        self.cursors.shared_index(i * 0x40)?.store(expected, Ordering::Release);
        crate::pgtable::clean_poc(self.cursors.as_slice()[i * 0x40..].as_ptr(), 0x40);
        crate::pgtable::poc_barrier();
        Ok(true)
    }

    /// Slot `i`'s record count (+0xc), its retirement cursor and whether the firmware still
    /// holds it in the queue's tracking list (+0x94), for a stall report.
    pub(crate) fn describe(&self, i: usize) -> Result<(u32, u32, u32)> {
        if i >= POOL_SLOTS { return Err(EINVAL); }
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[i * 0x100..].as_ptr(), 0x100);
        crate::pgtable::clean_invalidate_poc(self.cursors.as_slice()[i * 0x40..].as_ptr(), 0x40);
        crate::pgtable::poc_barrier();
        Ok((self.tracking.shared_index(i * 0x100 + 0xc)?.load(Ordering::Acquire),
            self.cursors.shared_index(i * 0x40)?.load(Ordering::Acquire),
            self.tracking.shared_index(i * 0x100 + 0x94)?.load(Ordering::Acquire)))
    }

    /// Whether the firmware still holds slot `i` in the queue's tracking list (+0x94).
    pub(crate) fn registered(&self, i: usize) -> Result<bool> {
        if i >= POOL_SLOTS { return Err(EINVAL); }
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[i * 0x100..].as_ptr(), 0x100);
        crate::pgtable::poc_barrier();
        Ok(self.tracking.shared_index(i * 0x100 + 0x94)?.load(Ordering::Acquire) != 0)
    }
}

pub(crate) enum FirmwareEvent {
    /// Kind 0: the firmware stopped on a GPU fault and halted for recovery.
    Fault,
    /// Kind 4: the firmware timed out a command (the slot it names at +0xc) and halted for
    /// recovery.
    Timeout { slot: i32 },
    Grow(GrowRequest),
    ChannelError,
    Notification,
}

pub(crate) struct ControlReply<'a> {
    state: &'a fw::types::GpuArray<u8>,
    ring: &'a fw::types::GpuArray<u8>,
    cursor: u32,
}

impl ControlReply<'_> {
    pub(crate) fn grow(self, request: GrowRequest, granted: bool) -> Result {
        let record = request.reply(granted);
        let at = self.cursor as usize * core::mem::size_of_val(&record);
        for (index, word) in record.into_iter().enumerate() {
            self.ring.shared_index(at + index * 4)?.store(word, Ordering::Relaxed);
        }
        crate::pgtable::clean_poc(self.ring.as_slice()[at..].as_ptr(), core::mem::size_of_val(&record));
        crate::pgtable::poc_barrier();
        self.state.shared_index(0x20)?.store((self.cursor + 1) % RING_RECORDS, Ordering::Release);
        crate::pgtable::clean_poc(self.state.as_slice().as_ptr(), 0x30);
        crate::pgtable::poc_barrier();
        Ok(())
    }
}

/// The firmware 27.2 root and every record it names.
pub(crate) struct RootV27_2 {
    graph: Arc<GpuManagerG14V13_5>,
    boot: BootJob,
    compute_state: u64,
    render_states: [u64; 2],
    render_queues: [DmQueue; 2],
    channels: KVec<fw::types::GpuArray<u8>>,
    _owners: KVec<fw::types::GpuArray<u8>>,
    _register_mappings: KVec<mmu::KernelMapping>,
    _controller: fw::types::GpuArray<u8>,
    _directory: fw::types::GpuArray<u8>,
    boot_page: fw::types::GpuArray<u8>,
    globals: fw::types::GpuArray<u8>,
    status: fw::types::GpuArray<u8>,
    control: fw::types::GpuArray<u8>,
    power: fw::types::GpuArray<u8>,
    extra: fw::types::GpuArray<u8>,
}

impl fw::types::GpuStruct for RootV27_2 {
    type Raw<'a> = fw::initdata::raw::InitDataG14V27_2<'a>;
}

impl GpuManagerG14V13_5 {
    /// The kernel UAT these records live in.
    pub(crate) fn kernel_uat(&self) -> &mmu::Uat {
        &self.uat
    }

    /// The firmware timestamp aperture that timestamp buffers are bound in.
    pub(crate) fn timestamp_range(&self) -> Range<u64> {
        self.timestamp_range.clone()
    }
}

impl RootV27_2 {
    fn word(a: &fw::types::GpuArray<u8>, at: usize) -> Result<u32> {
        Ok(a.shared_index(at)?.load(Ordering::Acquire))
    }

    /// The queue's identity for client commands, and the stamp value it last reached.
    pub(crate) fn queue(&self) -> (QueueInfo, event::EventValue) {
        (self.boot.info(self.compute_state), self.boot.event.current_noncoherent())
    }

    /// The queue's capacity (position block +0x50).
    pub(crate) fn capacity(&self) -> Result<u32> {
        Self::word(&self.boot.positions, 0x50)
    }

    /// The queue head and the firmware's retired position, which it advances in order.
    pub(crate) fn progress(&self) -> Result<(u32, u32)> {
        let positions = self.boot.positions.as_slice();
        crate::pgtable::clean_invalidate_poc(positions.as_ptr(), 0x60);
        crate::pgtable::poc_barrier();
        Ok((Self::word(&self.boot.positions, 0x40)?, Self::word(&self.boot.positions, 0)?))
    }

    pub(crate) fn render_queue(&self, dm: usize) -> (QueueInfo, event::EventValue) {
        let queue = &self.render_queues[dm];
        (queue.info(self.render_states[dm]), queue.event.current_noncoherent())
    }

    /// Whether the render queue's retired position reached `target`, stamp or not: a command
    /// the firmware stopped for recovery leaves the queue without writing its stamp.
    pub(crate) fn render_retired(&self, dm: usize, target: u32) -> Result<bool> {
        let queue = &self.render_queues[dm];
        crate::pgtable::clean_invalidate_poc(queue.positions.as_slice().as_ptr(), 0x60);
        crate::pgtable::poc_barrier();
        Ok(Self::word(&queue.positions, 0)? == target)
    }

    pub(crate) fn render_done(&self, dm: usize, target: u32, stamp: event::EventValue) -> Result<bool> {
        let queue = &self.render_queues[dm];
        crate::pgtable::clean_invalidate_poc(queue.positions.as_slice().as_ptr(), 0x60);
        crate::pgtable::poc_barrier();
        Ok(Self::word(&queue.positions, 0)? == target && queue.event.current_noncoherent() >= stamp)
    }

    fn log_queue(
        dev: &device::Device, name: &str, positions: &fw::types::GpuArray<u8>,
        queue: &fw::types::GpuArray<u8>, stamp: &event::Event,
    ) {
        crate::pgtable::clean_invalidate_poc(positions.as_slice().as_ptr(), 0x60);
        crate::pgtable::clean_invalidate_poc(queue.as_slice().as_ptr(), 0x90);
        crate::pgtable::poc_barrier();
        dev_err!(dev, "GPU: {} queue head={:?} fetched={:?} retired={:?} scheduler={:?} stamp={:#x}\n",
            name, Self::word(positions, 0x40), Self::word(positions, 0x30),
            Self::word(positions, 0), Self::word(queue, 0x74), stamp.current_noncoherent().raw());
    }

    pub(crate) fn log_timeout(&self, dev: &device::Device) {
        Self::log_queue(dev, "compute", &self.boot.positions, &self.boot.queue, &self.boot.event);
        for (name, queue) in ["tiler", "fragment"].into_iter().zip(&self.render_queues) {
            Self::log_queue(dev, name, &queue.positions, &queue.queue, &queue.event);
        }
        let (generation, phase) = (self.halt_count(), self.recovery_phase());
        let (snapshot, entries) = self.recovery_snapshot();
        dev_err!(dev, "GPU: recovery generation={:?} phase={:?} snapshot ({} entries) {:x?}\n",
                 generation, phase, entries, &snapshot[..entries.min(snapshot.len())]);
    }

    /// The first valid recovery snapshot entries, {flags, target} in hex, and how many valid
    /// entries there are in all (the first invalid entry ends the snapshot). Logging only.
    pub(crate) fn recovery_snapshot(&self) -> ([(u32, u32); 4], usize) {
        let mut first = [(0, 0); 4];
        let mut entries = 0;
        for i in 0..RECOVERY_SNAPSHOT_ENTRIES {
            match self.recovery_entry(i) {
                Ok((flags, target)) if flags & 1 != 0 => {
                    if let Some(slot) = first.get_mut(entries) { *slot = (flags, target); }
                    entries += 1;
                }
                _ => break,
            }
        }
        (first, entries)
    }

    pub(crate) fn render_kick(dm: usize) -> u64 {
        MSG_TX_DOORBELL | (4 * RENDER_GROUP + dm) as u64
    }

    pub(crate) fn render_queue_announced(&self, dm: usize) -> bool {
        self.render_queues[dm].announced.load(Ordering::Acquire)
    }

    pub(crate) fn stage_render(&self, dm: usize, commands: &[u64]) -> Result<Submission<'_>> {
        let queue = self.render_queues.get(dm).ok_or(EINVAL)?;
        self.stage_work(&queue.positions, &queue.pointers, queue.info(0),
                        Some(&queue.announced), RENDER_GROUP, dm, commands)
    }

    pub(crate) fn stage_compute(&self, command: u64) -> Result<Submission<'_>> {
        self.stage_work(&self.boot.positions, &self.boot.pointers, self.boot.info(self.compute_state),
                        None, PRIORITY_GROUP, COMPUTE, &[command])
    }

    #[allow(clippy::too_many_arguments)]
    fn stage_work<'a>(
        &'a self, positions: &'a fw::types::GpuArray<u8>, pointers: &fw::types::GpuArray<u8>,
        queue: QueueInfo, announced: Option<&'a core::sync::atomic::AtomicBool>,
        group: usize, dm: usize, commands: &[u64],
    ) -> Result<Submission<'a>> {
        let ring_index = 3 * group + dm;
        let indices = &self.channels[2 * ring_index];
        let ring = &self.channels[2 * ring_index + 1];
        crate::pgtable::clean_invalidate_poc(positions.as_slice().as_ptr(), 0x60);
        crate::pgtable::clean_invalidate_poc(indices.as_slice().as_ptr(), 0x30);
        crate::pgtable::poc_barrier();

        let head = positions.shared_index(0x40)?;
        let old = head.load(Ordering::Acquire);
        let capacity = Self::word(positions, 0x50)?;
        let retired = Self::word(positions, 0)?;
        let first = announced.is_some_and(|first| !first.load(Ordering::Acquire));
        if capacity as usize > pointers.len() / 8 || (first && old != retired) {
            return Err(EIO);
        }
        let count = u32::try_from(commands.len()).map_err(|_| EINVAL)?;
        let target = g14_v27_2_abi::queue_head(capacity, old, retired, count).map_err(|error| {
            match error {
                QueueError::Geometry => EIO,
                QueueError::Full => EBUSY,
            }
        })?;

        let producer = indices.shared_index(0x20)?;
        let w = producer.load(Ordering::Acquire);
        let r = Self::word(indices, 0)?;
        if w >= RING_RECORDS || r >= RING_RECORDS { return Err(EIO); }
        let next = (w + 1) % RING_RECORDS;
        if next == r { return Err(EBUSY); }
        let slot = u8::try_from(queue.slot).map_err(|_| EINVAL)?;
        let entry = crate::m3::submission_entry(dm as u32, queue.queue,
            u16::try_from(target).map_err(|_| EINVAL)?, slot, first).map_err(|_| EINVAL)?;
        let base = w as usize * entry.len();
        for (index, &command) in commands.iter().enumerate() {
            let at = ((old as usize + index) % capacity as usize) * 8;
            pointers.shared_index(at)?.store(command as u32, Ordering::Relaxed);
            pointers.shared_index(at + 4)?.store((command >> 32) as u32, Ordering::Relaxed);
            crate::pgtable::clean_poc(pointers.as_slice()[at..].as_ptr(), 8);
        }
        for (index, word) in entry.chunks_exact(4).enumerate() {
            ring.shared_index(base + 4 * index)?.store(
                u32::from_le_bytes([word[0], word[1], word[2], word[3]]), Ordering::Relaxed);
        }
        crate::pgtable::clean_poc(ring.as_slice()[base..].as_ptr(), entry.len());
        crate::pgtable::poc_barrier();
        Ok(Submission { head, positions: positions.as_slice(), target, producer,
            indices: indices.as_slice(), next, announced })
    }

    /// Publishes the boot job, whose command and ring entry the root already holds. The GPU
    /// must be held powered (`operating_point`: the Fender state, nonzero index, no request
    /// pending) and the command service installed.
    pub(crate) fn publish_boot_job(&self, operating_point: u32) -> Result {
        let job = &self.boot;
        let index = operating_point & 0xf;
        if operating_point & 0x10 != 0 || index == 0 || index > self.graph.dyncfg.pwr.perf_max_pstate
            || self.idle_progress()? != [1; 3]
            || self.idle_acknowledgement()? != Self::word(&self.globals, 0xe40)?
            || !self.commands_installed()? {
            return Err(EAGAIN);
        }
        let producer = self.channels[2 * SUBMISSION_RING].shared_index(0x20)?;
        let head = job.positions.shared_index(0x40)?;
        if Self::word(&self.status, 0x14)? != 1 || producer.load(Ordering::Acquire) != 0
            || head.load(Ordering::Acquire) != 0 {
            return Err(EAGAIN);
        }
        crate::pgtable::poc_barrier();
        head.store(1, Ordering::Release);
        crate::pgtable::poc_barrier();
        producer.store(1, Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok(())
    }

    /// Whether the boot job has retired: ring consumed, queue read and retired, stamp reached.
    pub(crate) fn boot_job_complete(&self) -> Result<bool> {
        self.drain()?;
        let job = &self.boot;
        Ok(Self::word(&self.channels[2 * SUBMISSION_RING], 0)? == 1
            && Self::word(&job.positions, 0x30)? == 1
            && Self::word(&job.positions, 0)? == 1
            && job.event.current_noncoherent().raw() == job.expected_stamp)
    }

    /// The record at the head of the event ring: its cursor, its raw kind word and its decoding.
    pub(crate) fn peek_event(&self) -> Result<Option<(u32, u32, FirmwareEvent)>> {
        use crate::m3::m3_channel_format::RECEIVE_BYTES;
        let state = &self.channels[2 * EVENT_RING];
        let ring = &self.channels[2 * EVENT_RING + 1];
        crate::pgtable::clean_invalidate_poc(state.as_slice().as_ptr(), 0x30);
        crate::pgtable::poc_barrier();
        let (read, write) = (Self::word(state, 0)?, Self::word(state, 0x20)?);
        if read >= RING_RECORDS || write >= RING_RECORDS { return Err(EIO); }
        if read == write { return Ok(None); }
        let at = read as usize * RECEIVE_BYTES;
        crate::pgtable::clean_invalidate_poc(ring.as_slice()[at..].as_ptr(), RECEIVE_BYTES);
        crate::pgtable::poc_barrier();
        let word = |offset| Self::word(ring, at + offset);
        let kind = word(0)?;
        let event = match g14_v27_2_abi::event_kind(kind) {
            EventKind::Grow => FirmwareEvent::Grow(GrowRequest::decode([
                word(4)?, word(8)?, word(0x0c)?, word(0x10)?, word(0x14)?, word(0x18)?,
            ])),
            EventKind::Fault => FirmwareEvent::Fault,
            EventKind::Timeout => FirmwareEvent::Timeout {
                slot: word(g14_v27_2_abi::TIMEOUT_SLOT_OFFSET)? as i32,
            },
            EventKind::ChannelError => FirmwareEvent::ChannelError,
            EventKind::Notification => FirmwareEvent::Notification,
        };
        Ok(Some((read, kind, event)))
    }

    /// Validates a byte range of the control object before it is sliced or cache-maintained.
    fn control_range(control: &fw::types::GpuArray<u8>, from: usize, bytes: usize) -> Result {
        match from.checked_add(bytes) {
            Some(end) if end <= control.as_slice().len() && from % 4 == 0 => Ok(()),
            _ => Err(EINVAL),
        }
    }

    /// Reads control words from `from` into `out`.
    pub(crate) fn control_snapshot(&self, from: usize, out: &mut [u32]) -> Result {
        Self::control_range(&self.control, from, out.len() * 4)?;
        let at = self.control.as_slice()[from..].as_ptr();
        crate::pgtable::clean_invalidate_poc(at, out.len() * 4);
        crate::pgtable::poc_barrier();
        for (i, w) in out.iter_mut().enumerate() {
            *w = Self::word(&self.control, from + 4 * i)?;
        }
        Ok(())
    }

    /// Writes one control word and makes it visible to the firmware.
    pub(crate) fn control_store(&self, at: usize, value: u32) -> Result {
        Self::control_range(&self.control, at, 4)?;
        self.control.shared_index(at)?.store(value, Ordering::Release);
        crate::pgtable::clean_poc(self.control.as_slice()[at..].as_ptr(), 4);
        crate::pgtable::poc_barrier();
        Ok(())
    }

    pub(crate) fn consume_event(&self, cursor: u32) -> Result {
        let state = &self.channels[2 * EVENT_RING];
        state.shared_index(0)?.store((cursor + 1) % RING_RECORDS, Ordering::Release);
        crate::pgtable::clean_poc(state.as_slice().as_ptr(), 0x30);
        crate::pgtable::poc_barrier();
        Ok(())
    }

    /// The recovery phase. The firmware publishes 1 after it reported a fault or a command
    /// timeout and stopped every queue; it runs nothing until the host's answers.
    pub(crate) fn recovery_phase(&self) -> Result<u32> {
        let mut word = [0u32];
        self.control_snapshot(CONTROL_RECOVERY_PHASE, &mut word)?;
        Ok(word[0])
    }

    /// Answers a recovery phase (2 at phase 1, 0 at phase 3).
    pub(crate) fn answer_recovery(&self, phase: u32) -> Result {
        self.control_store(CONTROL_RECOVERY_PHASE, phase)
    }

    /// Snapshot entry `i` of the stopped work: (flags, target). Bit 0 of flags marks a valid
    /// entry; the first invalid one ends the snapshot.
    pub(crate) fn recovery_entry(&self, i: usize) -> Result<(u32, u32)> {
        if i >= RECOVERY_SNAPSHOT_ENTRIES { return Err(EINVAL); }
        let mut words = [0u32; 2];
        self.control_snapshot(CONTROL_RECOVERY_SNAPSHOT + 8 * i, &mut words)?;
        Ok((words[0], words[1]))
    }

    /// The recovery generation (u64). Growth requests carry the value current when sent.
    pub(crate) fn halt_count(&self) -> Result<u64> {
        const OFFSET: usize = CONTROL_RECOVERY_GENERATION;
        crate::pgtable::clean_invalidate_poc(self.control.as_slice()[OFFSET..].as_ptr(), 8);
        crate::pgtable::poc_barrier();
        Ok(u64::from(Self::word(&self.control, OFFSET)?)
            | (u64::from(Self::word(&self.control, OFFSET + 4)?) << 32))
    }

    pub(crate) fn reserve_control_reply(&self) -> Result<Option<ControlReply<'_>>> {
        let state = &self.channels[2 * crate::m3::DEVICE_CONTROL];
        crate::pgtable::clean_invalidate_poc(state.as_slice().as_ptr(), 0x30);
        crate::pgtable::poc_barrier();
        let write = Self::word(state, 0x20)?;
        let consumers = [Self::word(state, 0)?, Self::word(state, 0x10)?];
        if write >= RING_RECORDS || consumers.iter().any(|&cursor| cursor >= RING_RECORDS) {
            return Err(EIO);
        }
        let next = (write + 1) % RING_RECORDS;
        if consumers.contains(&next) { return Ok(None); }
        Ok(Some(ControlReply { state, ring: &self.channels[2 * crate::m3::DEVICE_CONTROL + 1],
            cursor: write }))
    }

    /// Drain the telemetry rings. The event ring is consumed separately after servicing each event.
    pub(crate) fn drain(&self) -> Result {
        let consume = |state: &fw::types::GpuArray<u8>, base: usize, records: u32| -> Result {
            let write = Self::word(state, base + 0x20)?;
            let read = state.shared_index(base)?;
            if write < records && read.load(Ordering::Acquire) != write {
                read.store(write, Ordering::Release);
                crate::pgtable::poc_barrier();
            }
            Ok(())
        };
        consume(&self.channels[2 * STATISTICS_RING], 0, RING_RECORDS)?;
        // One log ring per log thread, state blocks 0x30 apart.
        for thread in 0..directory26::LOG_THREADS {
            consume(&self.channels[2 * LOG_RINGS], thread * 0x30, RING_RECORDS)?;
        }
        consume(&self.channels[2 * TRACE_RING], 0, TRACE_RECORDS)
    }

    /// Shared status +0x14: the firmware sets it to 1 once it has initialized.
    pub(crate) fn initialized(&self) -> Result<bool> {
        Ok(Self::word(&self.status, 0x14)? == 1)
    }

    /// Shared status +0x10: nonzero while the firmware reports itself running.
    pub(crate) fn firmware_running(&self) -> Result<u32> {
        Self::word(&self.status, 0x10)
    }

    /// Whether the machine's DT/ADT supplied the GPU power-controller tuning parameters.
    pub(crate) fn has_pwr_controller(&self) -> bool {
        self.graph.dyncfg.pwr.pwr_controller_from_dt
    }

    /// Whether the firmware has installed its command handlers: it clears the installation
    /// request (+0x4c24), which the root sets, once powered initialization has copied them.
    /// Engine entries may be published only after that.
    pub(crate) fn commands_installed(&self) -> Result<bool> {
        Ok(Self::word(&self.control, 0x4c24)? == 0)
    }

    pub(crate) fn idle_acknowledgement(&self) -> Result<u32> {
        Self::word(&self.control, 0xe838)
    }

    /// Device-control ring consumer, target and producer.
    pub(crate) fn idle_progress(&self) -> Result<[u32; 3]> {
        self.drain()?;
        let ring = &self.channels[2 * crate::m3::DEVICE_CONTROL];
        Ok([Self::word(ring, 0)?, Self::word(ring, 0x10)?, Self::word(ring, 0x20)?])
    }

    /// Publishes an idle action and its acknowledged generation on an idle control channel.
    pub(crate) fn publish_idle_policy(&self, allow_off: bool) -> Result<(u32, u32)> {
        let host = self.globals.shared_index(0xe40)?;
        let firmware = self.idle_acknowledgement()?;
        let producer = self.channels[2 * crate::m3::DEVICE_CONTROL].shared_index(0x20)?;
        if Self::word(&self.status, 0x14)? != 1 { return Err(EAGAIN); }
        let cursor = crate::m3::idle_cursor(self.idle_progress()?).map_err(|_| EBUSY)?;
        let action = crate::m3::Action::Idle { enabled: allow_off };
        let generation = action.generation(host.load(Ordering::Acquire), firmware).map_err(|_| EOVERFLOW)?;
        let mut record = [0; crate::m3::CONTROL_BYTES];
        action.encode(&mut record).map_err(|_| EINVAL)?;
        let ring = &self.channels[2 * crate::m3::DEVICE_CONTROL + 1];
        let at = cursor as usize * record.len();
        for (i, word) in record.chunks_exact(4).enumerate() {
            ring.shared_index(at + i * 4)?.store(
                u32::from_le_bytes([word[0], word[1], word[2], word[3]]), Ordering::Relaxed);
        }
        crate::pgtable::clean_poc(ring.as_slice()[at..].as_ptr(), record.len());
        host.store(generation, Ordering::Release);
        crate::pgtable::poc_barrier();
        let next = (cursor + 1) % RING_RECORDS;
        producer.store(next, Ordering::Release);
        crate::pgtable::poc_barrier();
        Ok((next, generation))
    }

    /// Builds the root, the boot job and every record they name.
    pub(crate) fn build(graph: &Arc<GpuManagerG14V13_5>, board: &BoardV27_2) -> Result<fw::types::GpuObject<Self>> {
        let cfg = graph.cfg;
        let g13 = cfg.gpu_gen == hw::GpuGen::G13;
        let pwr = &graph.dyncfg.pwr;
        let states = &pwr.perf_states;
        let id = &graph.dyncfg.id;
        // Inputs the controller below is built from, all from the DT and the ID registers.
        let [core_mask] = id.core_masks.as_slice() else { return Err(EINVAL); };
        if (!g13 && !pwr.power_zones.is_empty()) || pwr.power_zones.len() > 5
            || pwr.max_power_mw == 0 || pwr.max_power_mw > i32::MAX as u32
            || states.last().map(|s| s.pwr_mw) != Some(pwr.max_power_mw)
            || pwr.power_sample_period == 0
            || (pwr.pwr_controller_from_dt && pwr.pwr_filter_time_constant == 0)
            || pwr.ppm_filter_time_constant_ms < pwr.power_sample_period
            || pwr.avg_power_filter_tc_ms < pwr.power_sample_period
            || pwr.avg_power_target_filter_tc == 0
            || !(2..=abi::COMMON26_HW_PERF_ROWS).contains(&states.len())
            || states.windows(2).any(|s| s[1].pwr_mw <= s[0].pwr_mw || s[1].freq_hz <= s[0].freq_hz)
            || pwr.perf_base_pstate == 0 || pwr.perf_base_pstate as usize >= states.len()
            || core_mask.count_ones() != id.total_active_cores || id.total_active_cores == 0
            || (!g13 && cfg.shared3_tab.len() != 16)
            || board.copy_bytes == 0 || board.sgx_size < 0xe14000 {
            return Err(EINVAL);
        }
        // Alarm sensors: the chip's fast-die selection, also its sensor inventory.
        let sensors = cfg.fast_sensor_mask[0];
        if sensors == 0 || sensors >> 32 != 0 || cfg.fast_sensor_mask_alt[0] != sensors {
            return Err(EINVAL);
        }
        // The SRAM voltage of each operating point: published by the loader from the ADT on G14,
        // or derived from apple,min-sram-microvolt on G13 where the ADT has no perf-states-sram.
        let sram_table = Self::sram_table(states.len())?;
        if !g13 && sram_table.is_none() {
            return Err(EINVAL);
        }
        let min_sram_mv = pwr.min_sram_microvolt / 1000;
        if sram_table.is_none() && min_sram_mv == 0 {
            return Err(EINVAL);
        }

        let mut alloc = graph.alloc();
        graph.event_manager.sync_initial_stamps();
        let mut boot = BootJob::new(&mut alloc, graph.event_manager.try_get_polled()?,
                                    graph.kernel_uat(), &graph.dev, cfg)?;
        // The 0x60-byte root child and the eight-byte temperature generation occupy
        // consecutive, 64-byte-aligned shared suballocations.
        let extra = alloc.shared.array_empty::<u8>(0x80 + 8)?;

        // The thirteen TX descriptors and four RX rings.
        let mut channels = KVec::new();
        let mut addresses = [0; 34];
        for (index, address) in addresses.iter_mut().enumerate() {
            let size = directory26::allocation(index).map_err(|_| EINVAL)?.size;
            let owner = if index < 26 && index % 2 == 1 {
                alloc.private.array_empty::<u8>(size)?
            } else {
                alloc.shared.array_empty::<u8>(size)?
            };
            *address = owner.gpu_va().get();
            channels.push(owner, GFP_KERNEL)?;
        }
        {
            // The boot job's entry is pending at index 0 of its empty ring.
            let slot = u8::try_from(boot.event.slot()).map_err(|_| EINVAL)?;
            let entry = crate::m3::submission_entry(COMPUTE as u32, boot.queue.gpu_va().get(), 1, slot, true)
                .map_err(|_| EINVAL)?;
            let ring = channels[2 * SUBMISSION_RING + 1].as_mut_slice();
            ring[..entry.len()].copy_from_slice(&entry);
            crate::pgtable::clean_poc(ring.as_ptr(), ring.len());
        }


        let mut directory = alloc.private.array_empty::<u8>(0x4c8)?;
        let bytes = directory.as_mut_slice();
        directory26::write_channels(bytes, &addresses).map_err(|_| EINVAL)?;
        let base = &graph.initdata.runtime_pointers;

        // Hardware data: the 27.2 inline block.
        use fw::initdata::raw::HwDataBG14V27_2 as Hw;
        use crate::m3::m3_mmio_26::{write_record, RECORD_BYTES, TABLE_OFFSET};
        let mut hardware = alloc.private.array_empty::<u8>(0x2788)?;
        let hw = hardware.as_mut_slice();
        let mode = |slot: usize| if board.writable & (1 << slot) != 0 { 2 } else { 0 };
        for slot in 0..60 {
            write_record(&mut hw[TABLE_OFFSET + RECORD_BYTES * slot..], 0, 0, 0, 0, 0, mode(slot))
                .map_err(|_| EINVAL)?;
        }
        let mut register_mappings = KVec::new();
        let mut io_va = IOVA_KERN_MMIO_RANGE.start;
        for reg in board.registers {
            let physical = match reg.base {
                RegisterBaseV27_2::Physical(pa) => pa,
                RegisterBaseV27_2::Sgx(offset) => board.sgx + offset,
            };
            let Some(va_offset) = reg.install else { continue; };
            let subpage = physical as usize & mmu::UAT_PGMSK;
            let span = (subpage + reg.size as usize + mmu::UAT_PGMSK) & !mmu::UAT_PGMSK;
            let writable = mode(reg.slot) == 2;
            let page_start = physical - subpage as u64;
            let va = match va_offset {
                0 => io_va,
                offset => IOVA_KERN_MMIO_RANGE.start + offset,
            };
            let page = graph.uat.kernel_vm().map_io(
                graph.dev.as_ref().as_ref(), va, page_start as usize, span,
                if writable { mmu::PROT_FW_MMIO_RW } else { mmu::PROT_FW_MMIO_RO },
            )?;
            if page.translate_offset(subpage)? != physical
                || page.translate_offset(subpage + reg.size as usize - 1)? != physical + u64::from(reg.size) - 1 {
                return Err(EINVAL);
            }
            write_record(&mut hw[TABLE_OFFSET + RECORD_BYTES * reg.slot..], physical,
                page.iova() + subpage as u64, reg.size, reg.size, reg.relative, mode(reg.slot))
                .map_err(|_| EINVAL)?;
            if va_offset == 0 {
                io_va = io_va.checked_add(span as u64).ok_or(EOVERFLOW)?;
            }
            register_mappings.push(page, GFP_KERNEL)?;
        }
        const _: () = assert!(core::mem::offset_of!(Hw, coredump_copy_buffer_size) + 8 <= 0xfc0);
        const _: () = assert!(core::mem::offset_of!(Hw, chip_id) + 16 <= 0xfc0);
        let dump = core::mem::offset_of!(Hw, coredump_copy_buffer_size);
        put64(hw, dump, u64::from(board.copy_bytes));
        base.hwdata_b.with(|old, _| {
            for (offset, value) in [
                (0x00, old.unk_8.0), (0x08, old.unk_18.0),
                (0x10, 0x11_00000000), (0x18, 0x11_00000000),
                (0x20, 0x6f_ffff8000), (0x28, IOVA_G14_V27_2_TIMESTAMP_RANGE.start),
                (0x10cc, old.uat_ttb_base.0),
            ] {
                put64(hw, offset, value);
            }
            for (offset, value) in [(0x10d4, old.gpu_core_id), (0x10d8, old.gpu_rev_id),
                                    (0x10dc, id.total_active_cores), (0x10e0, old.max_pstate)] {
                put32(hw, offset, value);
            }
            let chip = core::mem::offset_of!(Hw, chip_id);
            for (i, word) in [old.chip_id, old.unk_454, old.unk_458, old.unk_45c].into_iter().enumerate() {
                put32(hw, chip + 4 * i, word);
            }
            if !g13 {
                // From the power sample period to the fragment count, the 27.2 block keeps the 13.5
                // HwDataB words in order, at the chip id's distance: the firmware's power sampling
                // period, the cache-flush and feature enables and the fragment count.
                use core::mem::offset_of;
                type Old = fw::initdata::raw::HwDataBG14V13_5;
                let shift = chip - offset_of!(Old, chip_id);
                for (at, value) in [
                    (offset_of!(Old, power_sample_period), old.power_sample_period),
                    (offset_of!(Old, unk_49c), old.unk_49c), (offset_of!(Old, unk_4a0), old.unk_4a0),
                    (offset_of!(Old, unk_4a4), old.unk_4a4), (offset_of!(Old, unk_4c0), old.unk_4c0),
                    (offset_of!(Old, unk_4f0), old.unk_4f0), (offset_of!(Old, unk_4f4), old.unk_4f4),
                    (offset_of!(Old, unk_504), old.unk_504), (offset_of!(Old, unk_524), old.unk_524),
                    (offset_of!(Old, unk_534), old.unk_534), (offset_of!(Old, num_frags), old.num_frags),
                ] {
                    put32(hw, at + shift, value);
                }
                put64(hw, offset_of!(Old, unk_4e0) + shift, old.unk_4e0.0);
            }
        });
        if g13 {
            // Inline words for G13G (T8103) firmware 27.2.1 (26B131): derived from the 13.5
            // configuration and the chip's register capabilities (no CRE on T8103).
            for (at, v) in [
                // Sleep/wake handler and RTKit power callback enables matching G14.
                (0xfc0, 1u32), (0xfc4, 1), (0xfd8, 1), (0xfdc, 1), (0xfe4, 1), (0xfec, 24000),
                (0xff0, 1), (0xff4, pwr.power_sample_period), (0xffc, 1), (0x1000, 1), (0x1004, 1),
                (0x1020, 0x1f), (0x1050, 1), (0x1054, 1), (0x1068, 0x31), (0x1088, 1),
                (0x10a4, id.num_frags * id.num_clusters), (0x10a8, 1),
                (0x2664, cfg.max_num_gps), (0x2668, 3), (0x266c, 7), (0x2670, 7),
                (0x267c, 0), (0x2680, 0), (0x2684, id.total_active_cores),
                (0x268c, 1), (0x26a8, 0xe_0000),
                (0x26b0, 1), (0x26b4, 1), (0x26b8, 0), (0x26c0, 1), (0x26c4, 1),
                (0x277c, 0x1_0000), (0x2780, 0x1_0000),
            ] {
                put32(hw, at, v);
            }
            hw[0x26d0..0x2700].fill(0xff);
        } else {
            // Inline words the stock host producer fixes for a gpu,t8112 node without model-slow.
            for (offset, value) in [(0xfc8u32, 0u32), (0xfd8, 1), (0xfec, 24000), (0xff0, 1),
                                    (0xffc, 1), (0x2724, 1), (0x2754, 1),
                                    (0xfc4, 1), (0xfdc, 1), (0xfe0, 0), (0x1028, 0),
                                    (0x109c, 0), (0x1064, 0), (0x2728, 0)] {
                put32(hw, offset as usize, value);
            }
            // Per-engine kick masks. The firmware gives an engine a kick slot only for bits set
            // here, so an engine whose mask is left zero is never scheduled. The tiler takes one
            // bit per cluster; the fragment and compute engines take one bit per configured core.
            let low_bits = |n: u32| if n < 32 { (1u32 << n) - 1 } else { u32::MAX };
            put32(hw, 0x2668, low_bits(id.num_clusters));
            for offset in [0x266c, 0x2670] {
                put32(hw, offset, low_bits(id.total_active_cores));
            }
            // Firmware-managed operating points, CRE and event-assisted operating-point completion.
            for offset in [0xfc0, 0x26b8, 0x1054] {
                put32(hw, offset, 1);
            }
            put64(hw, 0x267c, u64::from(*core_mask));
            put32(hw, 0x2684, id.total_active_cores);
        }
        // Operating points, in the 13.5 HwDataB order the 27.2 block keeps: power above the
        // lowest state and frequency above the base state as f32 percentages, MHz, the per-lane
        // core and SRAM voltages, the chip's SRAM power coefficient per state and the identity
        // state index map.
        let top = states.len() - 1;
        if old_max_pstate(base)? as usize != top { return Err(EINVAL); }
        let baseline = states[0].pwr_mw;
        let span = crate::float::F32::from(states[top].pwr_mw - baseline);
        for (i, state) in states.iter().enumerate() {
            let percentage = crate::float::F32::from(state.pwr_mw - baseline) * crate::f32!(100.0) / span;
            put32(hw, abi::COMMON26_HW_PERF_RELATIVE_A + i * 4, percentage.to_bits());
        }
        let base_idx = pwr.perf_base_pstate as usize;
        let base_hz = states[base_idx].freq_hz;
        let span = crate::float::F32::from(states[top].freq_hz - base_hz);
        for (i, state) in states.iter().enumerate().skip(base_idx + 1) {
            let relative = crate::float::F32::from(state.freq_hz - base_hz) * crate::f32!(100.0) / span;
            put32(hw, abi::COMMON26_HW_PERF_RELATIVE_B + 4 * i, relative.to_bits());
        }
        for (i, state) in states.iter().enumerate() {
            let [mv] = state.volt_mv.as_slice() else { return Err(EINVAL); };
            let sram_mv = if let Some(table) = sram_table.as_ref() {
                let (freq, voltage) = table[i * 8..i * 8 + 8].split_at(4);
                let value = u32::from_le_bytes(voltage.try_into().map_err(|_| EINVAL)?);
                if u32::from_le_bytes(freq.try_into().map_err(|_| EINVAL)?) != state.freq_hz || value < *mv || value == 0 {
                    return Err(EINVAL);
                }
                value
            } else {
                (*mv).max(min_sram_mv)
            };
            let mhz = state.freq_hz / 1_000_000;
            put32(hw, abi::COMMON26_HW_PERF_FREQUENCY + i * 4, mhz);
            put32(hw, abi::COMMON26_HW_PERF_SCALE + i * 4, cfg.sram_k.to_bits());
            let sram_lanes = if g13 { 1 } else { abi::COMMON26_HW_PERF_LANES };
            for lane in 0..abi::COMMON26_HW_PERF_LANES {
                let row = i * abi::COMMON26_HW_PERF_ROW_STRIDE + lane * 4;
                put32(hw, abi::COMMON26_HW_PERF_CORE_VOLTAGE + row, *mv);
                if lane < sram_lanes {
                    put32(hw, abi::COMMON26_HW_PERF_SRAM_VOLTAGE + row, sram_mv);
                }
            }
        }
        for index in 0..abi::COMMON26_HW_PERF_ROWS {
            put32(hw, abi::COMMON26_HW_PERF_INDEX_MAP + index * 4, index as u32);
        }
        crate::pgtable::clean_poc(hw.as_ptr(), hw.len());
        put64(bytes, 0, hardware.gpu_va().get());
        channels.push(hardware, GFP_KERNEL)?;

        // The directory's other records; +0x485 is the power controller.
        let mut owners = KVec::new();
        let mut controller = None;
        let mut compute_state = 0;
        let mut render_states = [0; 2];
        for (offset, size) in [(0x10, 0x88), (0x254, 0x678), (0x25c, 0x760),
                               (0x264, 0x6d0), (0x26c, 0x60), (0x485, CONTROLLER_BYTES)] {
            let mut owner = alloc.private.array_empty::<u8>(size)?;
            if offset == 0x485 {
                Self::controller(owner.as_mut_slice(), graph, &extra, sensors)?;
            }
            let address = owner.gpu_va().get();
            put64(bytes, offset, address);
            if offset == 0x10 {
                put64(bytes, 8, address);
            }
            if offset == 0x254 { render_states[TILER] = address; }
            if offset == 0x25c { render_states[FRAGMENT] = address; }
            if offset == 0x264 {
                // The compute engine state the start and finish records name.
                compute_state = address;
            }
            if offset == 0x485 {
                controller = Some(owner);
            } else {
                owners.push(owner, GFP_KERNEL)?;
            }
        }
        let controller = controller.ok_or(EINVAL)?;
        let ids = alloc.private.array_empty::<u8>(0x134c)?;
        for (slot, offset) in [(0x2c8, 0), (0x2d0, 0x1000), (0x2d8, 0x1300)] {
            put64(bytes, slot, ids.gpu_va().get() + offset);
        }
        owners.push(ids, GFP_KERNEL)?;
        let low = base.buffer_mgr_ctl_low_mapping.as_ref().ok_or(EINVAL)?;
        let high = base.buffer_mgr_ctl_high_mapping.as_ref().ok_or(EINVAL)?;
        if low.translate_offset(0)? != high.translate_offset(0)? { return Err(EINVAL); }
        put64(bytes, 0x2e0, low.iova());
        put64(bytes, 0x2e8, high.iova());
        bytes[0x3e4] = 0xff;
        let payload = alloc.shared.array_empty::<u8>(
            directory26::LOG_THREADS * RING_RECORDS as usize * directory26::LOG_PAYLOAD_BYTES)?;
        put64(bytes, 0x200, payload.gpu_va().get());
        owners.push(payload, GFP_KERNEL)?;
        crate::pgtable::clean_poc(bytes.as_ptr(), bytes.len());

        let status = alloc.shared.array_empty::<u8>(0x20)?;
        let boot_page = {
            let page = alloc.shared_ro.alloc_generic::<u8>(mmu::UAT_PGSZ, mmu::UAT_PGSZ, None)?;
            fw::types::GpuArray::empty(page, mmu::UAT_PGSZ)?
        };
        let mut globals = alloc.shared_ro.array_empty::<u8>(g14_v27_2_abi::GLOBALS_BYTES)?;
        {
            let data = globals.as_mut_slice();
            for (offset, value) in [(0x58, 0xffffu16), (0x5a, 0x28), (0x5c, 0xffff)] {
                data[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
            }
            put32(data, 0x6e, 1);
            let ceiling = pwr.perf_max_pstate * 100;
            if g13 {
                for (at, value) in [
                    (0x24, 3000u32), (0x30, 1), (0x38, 1), (0x40, 120),
                    (0x60, 0x1_0000), (0x64, 0x1_0000), (0x68, 0x3_0000), (0x78, 1),
                    (0x84, pwr.max_power_mw), (0x88, ceiling), (0x8c, ceiling), (0x98, ceiling),
                    (0x128, u32::MAX), (0x18c, 0xffff), (0x1b4, 0x800), (0x1b8, 0x1555),
                    (0x1d0, u32::MAX), (0x1e8, u32::MAX), (0x6bc, 0xc0_0007),
                    (0x7ec, 1), (0x7f0, 1), (0x7f4, 100), (0x7f8, 100),
                    (0x990, 400_000), (0x998, 40), (0x99c, 10), (0x9a0, 250), (0x9a4, 1),
                    (0x9b8, 2), (0x9bc, 40), (0x9c0, 5), (0x9c8, 40), (0x9cc, 50),
                    (0x9d0, 1), (0x9d8, 50), (0xe08, 250), (0xe28, 3), (0xe2c, 1),
                ] {
                    put32(data, at, value);
                }
                data[0xec..0x128].fill(0xff);
                data[0x1bc..0x1d0].fill(0xff);
                data[0x1dc..0x1e8].fill(0xff);
            } else {
                for (at, value) in [(0x84, pwr.max_power_mw), (0x88, ceiling), (0x8c, ceiling), (0x98, ceiling)] {
                    put32(data, at, value);
                }
                // The shared power tables the 13.5 InitData built from the chip configuration and
                // the DT operating points (the G14 curves scale with each state's frequency and
                // voltage). Left zero, the firmware runs the upper states without them.
                base.hwdata_a.with(|a, _| {
                    put_shared1(data, GLOBALS_SHARED1, &a.hws1);
                    put_shared2(data, GLOBALS_SHARED2, &a.hws2);
                });
                // The firmware's progress checks and compute watchdog, as M3 and M4 27.2 set them.
                for (at, value) in g14_v27_2_abi::GLOBALS_PROGRESS {
                    put32(data, at, value);
                }
            }
            crate::pgtable::clean_poc(data.as_ptr(), data.len());
        }
        let mut control = alloc.shared.array_empty::<u8>(0xe850)?;
        {
            // One shared backing: 0x20 bytes of indices, followed at +0x40 by 256 cache-flush
            // requests. An empty ring still needs both VAs.
            let flush = alloc.shared.array_empty::<u8>(0x40 + 256 * 0x18)?;
            let state = flush.gpu_va().get();
            directory26::write_flush_ring(control.as_mut_slice(), 0x4be0, state, state + 0x40)
                .map_err(|_| EINVAL)?;
            // Request command-handler installation during powered initialization.
            put32(control.as_mut_slice(), 0x4c24, 1);
            owners.push(flush, GFP_KERNEL)?;
        }
        // The firmware window mirrors only the kernel page tables that exist when it is
        // installed. Keep one allocation of the GPU read-write range alive from here, so the
        // tables that later hold the parameter-buffer lists are already visible to the firmware.
        owners.push(alloc.gpu.array_empty::<u8>(0x4000)?, GFP_KERNEL)?;
        let mut power = alloc.shared.array_empty::<u8>(0x6a0)?;
        {
            let data = power.as_mut_slice();
            if g13 {
                let period = pwr.power_sample_period;
                let dt = crate::float::F32::from(period) / crate::f32!(1000.0);
                let zone = pwr.power_zones.first();
                for (at, value) in [
                    (0x0c, 0u32),
                    (0x10, pwr.avg_power_filter_tc_ms / period),
                    (0x14, (pwr.avg_power_ki_only * dt).to_bits()),
                    (0x18, pwr.avg_power_kp.to_bits()),
                    (0x1c, pwr.avg_power_min_duty_cycle),
                    (0x20, pwr.avg_power_target_filter_tc),
                    (0x24, zone.map_or(0, |z| z.target)),
                    (0x28, zone.map_or(0, |z| z.target.saturating_sub(z.target_offset))),
                    (0x2c, zone.map_or(0, |z| z.filter_tc)),
                    (0xc8, cfg.da.unk_8cc),
                    (0xcc, 8000),
                    (0xd0, cfg.da.unk_87c as u32),
                    (0xd8, pwr.fast_die0_proportional_gain.to_bits()),
                    (0xdc, (pwr.fast_die0_integral_gain * dt).to_bits()),
                    (0xf0, 9900), (0xf4, 200),
                    (0x108, crate::f32!(100.0).to_bits()), (0x10c, crate::f32!(400.0).to_bits()),
                    (0x110, 200),
                    (0x12c, pwr.max_power_mw),
                    (0x130, pwr.ppm_kp.to_bits()),
                    (0x134, (pwr.ppm_ki * dt).to_bits()),
                    (0x5e0, 1), (0x5e4, 0x1b),
                    (0x624, crate::f32!(15.0).to_bits()), (0x628, crate::f32!(5.0).to_bits()),
                    (0x62c, 32), (0x630, pwr.perf_max_pstate * 100), (0x640, 500), (0x68c, 1),
                ] {
                    put32(data, at, value);
                }
            } else {
                // Cold mirrors of the controller's power and temperature-loop values.
                let gains = base.hwdata_a.with(|a, _| (a.ppm_kp.to_bits(), a.ppm_ki_dt.to_bits()));
                for (at, value) in [(0x12c, pwr.max_power_mw), (0x130, gains.0), (0x134, gains.1)] {
                    put32(data, at, value);
                }
                // The average-power filter and the third shared power table, as the 13.5 Globals
                // carries them (built from the DT power values and the chip configuration).
                graph.initdata.globals.with(|g, _| {
                    for (at, value) in [
                        (0x10, g.avg_power_filter_tc_periods), (0x14, g.avg_power_ki_dt.to_bits()),
                        (0x18, g.avg_power_kp.to_bits()), (0x1c, g.avg_power_min_duty_cycle),
                        (0x20, g.avg_power_target_filter_tc),
                    ] {
                        put32(data, at, value);
                    }
                    put_shared3(data, POWER_SHARED3, &g.hws3);
                });
                let initial = controller.as_slice();
                for (dst, src) in [(0xc8, 0x22d4), (0xcc, 0x2280), (0xd0, 0x2284),
                                   (0xd4, 0x22cc), (0xd8, 0x22b4), (0xdc, 0x22a4)] {
                    data[dst..dst + 4].copy_from_slice(&initial[src..src + 4]);
                }
            }
            crate::pgtable::clean_poc(data.as_ptr(), data.len());
        }

        let render_queues = [
            DmQueue::new(&mut alloc, graph.event_manager.try_get_polled()?, RENDER_GROUP)?,
            DmQueue::new(&mut alloc, graph.event_manager.try_get_polled()?, RENDER_GROUP)?,
        ];
        boot.encode(&boot.info(compute_state));
        let (page_size, page_bits, levels, descriptors) = graph.initdata.with(|r, _|
            (r.uat_page_size, r.uat_page_bits, r.uat_num_levels, r.uat_level_info));
        let root = alloc.shared_ro.alloc_generic::<Self>(mmu::UAT_PGSZ, mmu::UAT_PGSZ, None)?;
        core::mem::drop(alloc);
        let root = fw::types::GpuObject::<Self>::new_init_prealloc(root,
            |_| try_init!(Self {
                graph: graph.clone(), boot, compute_state, render_states, render_queues, channels, _owners: owners,
                _register_mappings: register_mappings, _controller: controller, _directory: directory,
                boot_page, globals, status, control, power, extra,
            }),
            |owners, _| {
                try_init!(fw::initdata::raw::InitDataG14V27_2 {
                    ver_info: fw::types::Array::new(core::array::from_fn(|i| (FIRMWARE_WORD >> (16 * i)) as u16)),
                    unk_buf: owners.boot_page.gpu_pointer(),
                    unk_8: 0,
                    unk_c: 0,
                    runtime_pointers: owners._directory.gpu_pointer(),
                    globals: owners.globals.gpu_pointer(),
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
        )?;
        root.with(|r, _| crate::pgtable::clean_poc(core::ptr::from_ref(r).cast(), core::mem::size_of_val(r)));
        Ok(root)
    }

    /// The loader's `adt-perf-states-sram` table under `/chosen/aurora-gpu-resources/sgx`
    /// (frequency and SRAM millivolts per state), if present.
    fn sram_table(states: usize) -> Result<Option<KVec<u8>>> {
        let chosen = kernel::of::chosen().ok_or(ENODEV)?;
        let is = |name: Option<KVec<u8>>, want: &[u8]| {
            name.as_deref().is_some_and(|n| n.strip_suffix(b"\0").unwrap_or(n) == want)
        };
        for parent in chosen.children() {
            if !is(parent.get_opt_property::<KVec<u8>>(c"name")?, b"aurora-gpu-resources") { continue; }
            for child in parent.children() {
                if is(child.get_opt_property::<KVec<u8>>(c"name")?, b"sgx") {
                    let Some(table) = child.get_opt_property::<KVec<u8>>(c"adt-perf-states-sram")? else {
                        return Ok(None);
                    };
                    if table.len() != states * 8 { return Err(EINVAL); }
                    return Ok(Some(table));
                }
            }
        }
        Ok(None)
    }

    /// The power and thermal controller (directory +0x485): the 13.5 calibrated values of the
    /// hardware data A, moved to the 27.2 layout.
    fn controller(policy: &mut [u8], graph: &GpuManagerG14V13_5, extra: &fw::types::GpuArray<u8>,
                  sensors: u64) -> Result {
        let pwr = &graph.dyncfg.pwr;
        let cfg = graph.cfg;
        let base = &graph.initdata.runtime_pointers;
        let max_state = (pwr.perf_states.len() as u32 - 1) * 100;
        for offset in [0x222c, 0x2880] {
            put32(policy, offset, max_state);
        }
        let generation = extra.gpu_va().get() + 0x80;
        if generation & 0x3f != 0 || extra.as_slice()[0x80..] != [0; 8] {
            return Err(EINVAL);
        }
        put64(policy, 0x3b98, generation);
        // Secure metrology sensor set, and the fast-die release temperature, mask and gains.
        put64(policy, 0x3ba0, sensors);
        let (release, mask, integral, proportional) = base.hwdata_a.with(|old, _|
            (old.fast_die0_release_temp_cc, old.fast_die0_sensor_mask.0,
             old.fast_die0_ki_dt.to_bits(), old.fast_die0_kp.to_bits()));
        for (source, value, width) in [(0x2270, u64::from(release), 4), (0x2268, mask, 8),
                                       (0x2294, u64::from(integral), 4), (0x22a4, u64::from(proportional), 4)] {
            let at = abi::relocate_by_bands(abi::COMMON26_POLICY_BANDS, source).ok_or(EINVAL)?;
            policy.get_mut(at..at + width).ok_or(EINVAL)?.copy_from_slice(&value.to_le_bytes()[..width]);
        }
        // Thermal thresholds, then the SoC-hot alarm selection: the chip's values, as the 13.5
        // InitData writes them (HwDataA unk_8cc, unk_87c and unk_e24).
        for (at, value) in [(0x22d4, cfg.da.unk_8cc), (0x2284, cfg.da.unk_87c as u32),
                            (0x2f30, cfg.da.unk_e24)] {
            put32(policy, at, value);
        }
        // Controller constants the 13.5 InitData also sets (unk_880, unk_894 = 1.0, unk_e28).
        const FILTER_STAGES: u32 = 4;
        const ALARM_ENABLE: u32 = 1;
        for (at, value) in [(0x2288, FILTER_STAGES), (0x229c, crate::f32!(1.0).to_bits()),
                            (0x2f34, ALARM_ENABLE)] {
            put32(policy, at, value);
        }
        // Per-sensor corrections from the chip's coefficient tables (13.5 unk_coef_a/unk_coef_b),
        // indexed from the first fast-die sensor id. G13 carries no per-sensor coefficient tables;
        // on chips that do, a sensor without a coefficient refuses.
        const FIRST_SENSOR: usize = 7;
        if let (Some(coef_a), Some(coef_b)) = (cfg.unk_coef_a.first(), cfg.unk_coef_b.first()) {
            for (at, coef) in [(0x2320, coef_a), (0x2420, coef_a), (0x2f38, coef_b), (0x3038, coef_b)] {
                for sid in (0..64usize).filter(|sid| sensors & (1u64 << sid) != 0) {
                    let value = sid.checked_sub(FIRST_SENSOR).and_then(|index| coef.get(index))
                        .ok_or(EINVAL)?;
                    put32(policy, at + 4 * sid, value.to_bits());
                }
            }
        } else if cfg.gpu_gen != hw::GpuGen::G13 {
            return Err(EINVAL);
        }
        put64(policy, 0x2f28, sensors);
        put32(policy, 0x3ba8, 1);
        // Keep the controller's per-state table and the words that control its use
        // together, as in the power record.
        base.hwdata_a.with(|a, _| put_shared3(policy, CONTROLLER_SHARED3, &a.hws3));
        // Warm-restart operands: base operating point and wake flags.
        for (at, value) in [
            (0x9c, 1),
            (0xa0, 1),
            (0xa4, pwr.perf_base_pstate),
            (0xa8, 1),
        ] {
            put32(policy, at, value);
        }
        for (source, bits) in [
            (4, pwr.pwr_sample_period_aic_clks),
            (8, pwr.pwr_sample_period_aic_clks),
            // Two initial state sources; the current state at +0x38 belongs to the firmware.
            (0x2c, pwr.perf_base_pstate),
            (0x30, pwr.perf_base_pstate),
            (0x9f0, pwr.pwr_min_duty_cycle),
            (0xb5c, pwr.perf_integral_gain.to_bits()),
            (0xb60, pwr.perf_integral_gain2.to_bits()),
            (0xb6c, pwr.perf_proportional_gain.to_bits()),
            (0xb70, pwr.perf_proportional_gain2.to_bits()),
        ] {
            let target = abi::relocate_by_bands(abi::COMMON26_POLICY_BANDS, source).ok_or(EINVAL)?;
            policy.get_mut(target..target + 4).ok_or(EINVAL)?.copy_from_slice(&bits.to_le_bytes());
        }
        // The calibrated values of the 13.5 producer; only the layout and the peak length unit
        // differ. No power zones.
        if pwr.pwr_controller_from_dt {
            // Power controller (reason 1): populated only when the machine's DT/ADT supplied
            // the tuning parameters, so long power-off replays (deep sleep or idle-off) keep
            // the limiter calibrated without falling back to built-in coefficients.
            let pwr_fields = base.hwdata_a.with(|a, _| [
                (0x9c8, a.unk_64c),
                (0x9d4, a.pwr_filter_a_neg.to_bits()),
                (0x9dc, a.pwr_filter_a.to_bits()),
                (0x9e4, a.pwr_integral_gain.to_bits()),
                (0x9ec, a.pwr_integral_min_clamp.to_bits()),
                (0x9f0, a.max_power_1.to_bits()),
                (0x9f4, a.pwr_proportional_gain.to_bits()),
                (0x9fc, a.pwr_pstate_related_k.to_bits()),
                (0xa04, max_state),
                (0xa08, a.max_pstate_scaled_2),
                (0xa14, a.max_power_2),
            ]);
            for (at, value) in pwr_fields {
                put32(policy, at, value);
            }
        }
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
            (0x28e8, 0), (0x28f0, a.max_power_4),
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
            put32(policy, at, value);
        }
        put64(policy, 0xae0, u64::from(samples) * u64::from(pwr.pwr_sample_period_aic_clks));
        crate::pgtable::clean_poc(policy.as_ptr(), policy.len());
        Ok(())
    }
}

/// The 13.5 hardware data's highest operating-point index.
fn old_max_pstate(base: &fw::initdata::RuntimePointersG14V13_5) -> Result<u32> {
    Ok(base.hwdata_b.with(|old, _| old.max_pstate))
}

impl GpuManagerG14V13_5 {
    pub(crate) fn new_render_buffer(&self, ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>) -> Result<buffer::BufferG14V13_5> {
        let mut alloc = self.alloc();
        buffer::BufferG14V13_5::new(
            self, &mut alloc, ualloc, ualloc_priv, &self.buffer_mgr,
            buffer::ParameterListPlacement::KernelGpu,
        )
    }
}
