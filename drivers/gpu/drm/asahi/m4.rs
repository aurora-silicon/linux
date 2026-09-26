// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::c_str;
use kernel::dma_fence::FenceObject;
use kernel::prelude::*;

/// Stamp poll budget per job. The self-test must not hold boot longer than 2x this.
pub(crate) const POLL_MS: i64 = 2000;

/// CL command slot (KB 30 §0).
pub(crate) const CMD_SIZE: usize = 0x880;

const START_CL: u32 = 11;
const START_CL_SIZE: usize = 0x1bc;
const WFI: u32 = 1;
const FINALIZE_CL: u32 = 12;
const FINALIZE_CL_SIZE: usize = 0x7c;
const END: u32 = 0x40000002;

const WFI_AT: usize = START_CL_SIZE;
const FINALIZE_AT: usize = WFI_AT + 4;
const END_AT: usize = FINALIZE_AT + FINALIZE_CL_SIZE;
const SKU_SIZE: usize = END_AT + 4;

/// Hardware VA form: 43 bits, bit 42 selects the upper (TTBR1) half. b000 masks its own register
/// values the same way (StartCL 0x198a8 `and x10, x10, #0x7ffffffffc0`, kick 0x27430).
pub(crate) const HW_VA_MASK: u64 = 0x7ff_ffff_ffff;

pub(crate) const CDM_CTL_G15: u64 = 0x1_5402_4201;
const CDM_CFG_1A458: u64 = 0x10c0_8860;
/// 0x107a0 (UscTpCdmResourceCfg0) with no threadgroup memory (desc+0x460 == 0).
const USC_TP_CDM_RES_CFG0: u64 = 0x00ff_0000;
const UMA_PTC: u64 = 0x0000_0060_0040_0020;
const DUPM_MIN_MAX: u64 = (96 * 9) << 32 | (32 * 9);

/// UMA dummy object size. b000 writes u32 +0x30 (0x1c3f8) and increments u64 +0x50
/// (0x1ab40..0x1ab48) through FinalizeCL+0x58; keep it well past +0x58.
pub(crate) const UMA_OBJ_SIZE: usize = 0x100;

pub(crate) struct Addrs {
    pub(crate) cmd: u64,
    pub(crate) sku: u64,
    /// StartCL context base X (GPU+FW RW, 16 KiB aligned). A and B from `ctx_sizes()`.
    pub(crate) ctx: u64,
    pub(crate) ctx_a: u64,
    pub(crate) ctx_b: u64,
    pub(crate) scratch: u64,
    pub(crate) scratch_p1: u64,
    pub(crate) cdm: u64,
    pub(crate) uma: u64,
    pub(crate) notifier: u64,
    pub(crate) notifier_buf: u64,
    pub(crate) stats: u64,
    pub(crate) work_queue: u64,
    pub(crate) host_stamp: u64,
    pub(crate) fw_stamp: u64,
    pub(crate) stamp_value: u32,
    pub(crate) stamp_slot: u32,
    pub(crate) queue_id: u32,
    /// Firmware-only completion test: pre-set cmd+0x878 so StartCL takes its skip path.
    pub(crate) skip: bool,
}

pub(crate) fn ctx_sizes(cores: u64, mgpus: u64) -> (u64, u64, u64) {
    let a = (cores * 0x1800 + (mgpus << 6) + 0x3ff) & !0x3ff;
    let b = mgpus << 11;
    (a, b, (2 * a + b) | 0x400)
}

/// GPU-side CL command. The bytes are the firmware struct; this type only keeps the allocation typed.
pub(crate) struct CmdBytes;

impl crate::object::GpuStruct for CmdBytes {
    type Raw<'a> = [u8; CMD_SIZE];
}

impl crate::fw::workqueue::Command for CmdBytes {}

/// Fence carried by the self-test job. Completion is observed by polling the stamp, not the fence.
pub(crate) struct M4Fence;

#[vtable]
impl kernel::dma_fence::FenceOps for M4Fence {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a kernel::str::CStr {
        c_str!("asahi")
    }

    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a kernel::str::CStr {
        c_str!("m4")
    }
}

pub(crate) static FENCE_KEY: Pin<&kernel::sync::LockClassKey> = kernel::static_lock_class!();

fn put_u32(buf: &mut [u8], off: usize, value: u32) -> Result {
    let end = off.checked_add(4).ok_or(EINVAL)?;
    buf.get_mut(off..end)
        .ok_or(EINVAL)?
        .copy_from_slice(&value.to_le_bytes());
    Ok(())
}

fn put_u64(buf: &mut [u8], off: usize, value: u64) -> Result {
    let end = off.checked_add(8).ok_or(EINVAL)?;
    buf.get_mut(off..end)
        .ok_or(EINVAL)?
        .copy_from_slice(&value.to_le_bytes());
    Ok(())
}

fn put_i32(buf: &mut [u8], off: usize, value: i32) -> Result {
    put_u32(buf, off, value as u32)
}

fn add(a: u64, b: u64) -> Result<u64> {
    a.checked_add(b).ok_or(EINVAL)
}

/// Build the 0x240-byte SKU stream.
pub(crate) fn build_sku(a: &Addrs) -> Result<KVec<u8>> {
    let mut sku = KVec::new();
    sku.resize(SKU_SIZE, 0, GFP_KERNEL)?;
    let b = sku.as_mut_slice();

    // StartCL (op-relative offsets; b000 handler 0x1969c uses x19 = op+4).
    put_u32(b, 0x00, START_CL)?;
    put_u64(b, 0x14, add(a.cmd, 0x20)?)?; // register array (b000 0x196a4: [op+0x14]+0x700)
    put_u64(b, 0x1c, a.stats)?; // stats[slot] record written at 0x1a94c..0x1a960
    put_u64(b, 0x24, a.work_queue)?;
    put_u32(b, 0x2c, 0)?;
    put_u32(b, 0x30, 1)?; // sets bit vm_slot of SGX+0xd8f0 (0x19778..0x197b0); upstream unk_28 = 1
    put_u32(b, 0x34, a.queue_id)?; // low 6 bits -> regs 0x10200/0x10428 (0x19710..0x19764)
    put_u32(b, 0x38, 0)?;
    put_u32(b, 0x3c, 0)?; // mirrors cmd+0x800; b000 only traces it (0x19910)
    put_u32(b, 0x40, 0)?; // !=0 would set the 0x288 mask and append 0x17d9 (0x19930)
    put_u64(b, 0x44, add(a.cmd, 0x76c)?)?; // job_params2
    put_u32(b, 0x50, 0)?; // uuid
    put_u64(b, 0x15c, a.uma)?; // UMA block; id 0xffffffff -> 0x34554 returns early
    let x = a.ctx;
    put_u64(b, 0x184, x)?;
    put_u64(b, 0x18c, add(x, a.ctx_a)?)?;
    put_u64(b, 0x194, add(x, 2 * a.ctx_a)?)?;
    put_u64(b, 0x19c, add(x, 2 * a.ctx_a + a.ctx_b)?)?;
    put_u64(b, 0x1a4, add(a.cmd, 0x878)?)?; // unk_flag pointer
    put_u64(b, 0x1ac, 1)?; // counter; skip if counter >= *notifier_buf (all-ff -> no skip)
    put_u64(b, 0x1b4, a.notifier_buf)?;

    put_u32(b, WFI_AT, WFI)?;

    let f = FINALIZE_AT;
    put_u32(b, f, FINALIZE_CL)?;
    put_u64(b, f + 0x04, a.stats)?;
    put_u64(b, f + 0x0c, a.work_queue)?;
    put_u32(b, f + 0x14, 0)?; // vm_slot
    put_u64(b, f + 0x18, add(a.cmd, 0x76c)?)?;
    put_u32(b, f + 0x24, 0)?;
    put_u64(b, f + 0x28, a.fw_stamp)?;
    put_u32(b, f + 0x30, a.stamp_value)?;
    // Pointer to StartCL's UMA slot, not the stream base (see header).
    put_u64(b, f + 0x58, add(a.sku, 0x15c)?)?;
    put_i32(b, f + 0x60, -(FINALIZE_AT as i32))?;
    b[f + 0x64] = 0;
    put_u64(b, f + 0x65, add(a.cmd, 0x878)?)?;
    put_u64(b, f + 0x6d, add(a.cmd, 0x860)?)?;

    put_u32(b, END_AT, END)?;
    Ok(sku)
}

/// Number of host register entries `fill_cmd` writes. b000 StartCL appends 8 more
/// (0x10201, 0x10428, 0x10099, 0x10091, 0xa5c1, 0xa5c9, 0x10229, 0x140a8), so the KRCE kick
/// count (gfx-data 0xe1d90, SGX+0xc048 bits 63:48) should read 30 = 0x1e.
pub(crate) const HOST_REGS: usize = 22;

/// Fill a zeroed 0x880-byte CL command (KB 30 §6.4).
pub(crate) fn fill_cmd(cmd: &mut [u8], a: &Addrs, sku_len: u32) -> Result {
    if cmd.len() != CMD_SIZE {
        return Err(EINVAL);
    }
    let m = |va: u64| va & HW_VA_MASK;
    put_u32(cmd, 0x000, 3)?;
    put_u64(cmd, 0x004, 1)?;
    put_u32(cmd, 0x010, 0)?; // vm_slot
    put_u64(cmd, 0x014, a.notifier)?;

    let s = a.scratch;
    let s1 = add(s, a.scratch_p1)?;
    let regs: [(u32, u64); HOST_REGS] = [
        (0x1a510, m(s)),
        (0x1a420, m(a.cdm)),
        (0x1a4d0, m(s1)),
        (0x1a4d8, m(s1 + 8)),
        (0x1a4e0, m(s1 + 16)),
        (0x1a4e8, m(s1 + 24)),
        (0x1a440, CDM_CTL_G15),
        (0x1a458, CDM_CFG_1A458),
        (0x12091, 0),
        (0x101d9, 0),
        (0x1a089, 0),
        (0x1a091, 0),
        (0x1a059, 0),
        (0x1a061, 0),
        (0x1a0b9, 0),
        (0x1a0c1, 0),
        (0x101d1, 0),
        (0xd479, 0),
        (0x1a0e9, 0),
        (0x107a1, USC_TP_CDM_RES_CFG0),
        (0xa599, UMA_PTC),
        (0xd411, DUPM_MIN_MAX),
    ];
    for (i, (reg, val)) in regs.iter().enumerate() {
        let off = 0x20 + i * 12;
        put_u32(cmd, off, *reg)?;
        put_u64(cmd, off + 4, *val)?;
    }
    let count = regs.len() as u32;
    put_u64(cmd, 0x720, add(a.cmd, 0x20)?)?;
    put_u32(cmd, 0x728, count | ((count * 12) << 16))?;

    put_u64(cmd, 0x760, a.sku)?;
    put_u32(cmd, 0x768, sku_len)?;

    put_u32(cmd, 0x76c, 0)?; // resume flag
    put_u32(cmd, 0x770, 0)?; // preempt-pointer swap flag
    put_u64(cmd, 0x774, 0)?;
    put_u64(cmd, 0x77c, 0)?;
    put_u64(cmd, 0x794, m(s))?;
    put_u64(cmd, 0x79c, m(a.cdm))?; // address OF the terminate word
    put_u64(cmd, 0x7c4, CDM_CTL_G15)?;

    put_u64(cmd, 0x7e8, a.host_stamp)?;
    put_u64(cmd, 0x7f0, a.fw_stamp)?;
    put_u32(cmd, 0x7f8, a.stamp_value)?;
    put_u32(cmd, 0x7fc, a.stamp_slot)?;
    put_u32(cmd, 0x800, 0)?; // evctl_index (upstream: fixed 0)
    put_u32(cmd, 0x804, 1)?; // flush_stamps
    put_u32(cmd, 0x808, 0)?; // uuid
    put_u64(cmd, 0x83e, a.uma)?;
    // unk_flag: 1 = firmware-only skip path (no hardware kick).
    put_u32(cmd, 0x878, if a.skip { 1 } else { 0 })?;
    Ok(())
}

const _: () = {
    assert!(START_CL_SIZE == 0x1bc);
    assert!(FINALIZE_CL_SIZE == 0x7c);
    assert!(SKU_SIZE == 0x240);
    assert!(SKU_SIZE % 64 == 0);
    assert!(END_AT + 4 == SKU_SIZE);
    assert!(0x20 + HOST_REGS * 12 <= 0x700);
    assert!(DUPM_MIN_MAX == 0x0000_0360_0000_0120);
};
