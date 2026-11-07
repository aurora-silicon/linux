// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Pure G16G/25G83 compute encoding. Publication and lifetime proofs belong to the engine.

use super::{compute, manager, put16, put32, put64, put8, uapi};

/// A rejected detached image is never publishable; all validation precedes writes.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum ComputeEncodeError {
    Context,
    Chronology,
    Address,
    Command,
    Attachment,
    #[allow(dead_code)]
    Storage,
    Descriptor,
}

/// Exact aliases and chronology of one retained compute unit. Client VAs and firmware VAs
/// are deliberately separate: every alias must resolve the same held backing before encoding.
#[derive(Copy, Clone, Default)]
pub(crate) struct ComputeRefs {
    pub(crate) notifier: u64,
    pub(crate) qinfo: u64,
    pub(crate) user_stamp: u64,
    pub(crate) fw_stamp: u64,
    pub(crate) work: u64,
    pub(crate) work_gva: u64,
    pub(crate) seq: u64,
    pub(crate) preempt_gva: u64,
    pub(crate) its: u64,
    pub(crate) stats_fw: u64,
    pub(crate) auxiliary_fw: u64,
    pub(crate) auxiliary_gva: u64,
    pub(crate) context_store_gva: u64,
    pub(crate) counter_fw: u64,
    pub(crate) pool_state_gva: u64,
    pub(crate) page_list_gva: u64,
    pub(crate) usc_base: u64,
    /// Zero selects `notifier`; otherwise the current job's exact notifier record.
    pub(crate) record_fw: u64,
    pub(crate) binding_generation: u8,
    pub(crate) uma_slot: u32,
    /// Native command descriptor identifier (work+0x820), independent of compute index.
    pub(crate) command_id: u64,
    /// Native u32 compute sequence/index (StartCompute+0x38).
    pub(crate) compute_index: u32,
    /// Opaque native work+0x818 value, copied separately to StartCompute+0x3c.
    pub(crate) work_818: u32,
    pub(crate) target_stamp: u32,
    /// Cumulative private UMA submissions; never derived from the engine stamp.
    pub(crate) submitted: u64,
}

const FW_START: u64 = 0xfffffc2000000000;
const FW_END: u64 = 0xfffffc2ffc000000;
const VM_START: u64 = 0x4000;
const VM_END: u64 = (1 << 42) - 0x8000;

fn range(va: u64, size: u64, alignment: u64, start: u64, end: u64) -> bool {
    va >= start && va < end && va & (alignment - 1) == 0 && size != 0 && size <= end - va
}

fn fw(va: u64, size: u64) -> bool {
    range(va, size, 8, FW_START, FW_END)
}

fn gpu(va: u64, size: u64, alignment: u64) -> bool {
    range(va, size, alignment, VM_START, VM_END)
}

/// Fresh private allocator initialization only. A reused allocator retains both arrays and
/// all firmware-mutated descriptor bytes. This helper neither maps nor publishes its storage.
#[allow(dead_code)]
pub(crate) fn init_compute_pool(
    pool_gva: u64,
    state: &mut [u8],
    page_list: &mut [u8],
) -> Result<(), ComputeEncodeError> {
    if state.len() != compute::POOL_STATE_SIZE || page_list.len() != compute::PAGE_LIST_SIZE {
        return Err(ComputeEncodeError::Storage);
    }
    if !gpu(pool_gva, compute::POOL_SIZE as u64, 0x4000) {
        return Err(ComputeEncodeError::Address);
    }
    state.fill(0);
    page_list.fill(0);
    for i in 0..8192 {
        put64(state, i * 8, pool_gva + i as u64 * 0x1000);
    }
    for i in 0..16 {
        put64(
            page_list,
            i * 64,
            (1 << 61) | (pool_gva + i as u64 * 0x200000),
        );
    }
    Ok(())
}

/// Restores the complete allocator descriptor into a newly encoded, unpublished work after
/// the engine has proved A retired, distinct B displaced A in the same UMA slot, and B retired.
/// Numeric checks here do not establish that ownership proof. The old descriptor's completion
/// counter must equal A's cumulative submission count. Pool/list/counter backing stays pinned;
/// only the work pointer and destination slot are rewritten. Returns the next private counter.
pub(crate) fn preserve_compute_descriptor(
    r: &ComputeRefs,
    old: &[u8; compute::DESCRIPTOR_SIZE],
    submitted_before: u64,
    w: &mut [u8; compute::WORK_SIZE],
) -> Result<u64, ComputeEncodeError> {
    fn read64(b: &[u8], off: usize) -> u64 {
        let mut v = [0; 8];
        v.copy_from_slice(&b[off..off + 8]);
        u64::from_le_bytes(v)
    }
    if submitted_before == 0
        || submitted_before == u64::MAX
        || r.submitted != submitted_before + 1
        || r.uma_slot == 0
        || r.uma_slot > 32
        || read64(old, manager::WORK) != r.work
        || read64(old, manager::FREELIST) != r.pool_state_gva
        || read64(old, manager::POOL_TABLE) != r.page_list_gva
        || read64(old, manager::COUNTER) != r.counter_fw
        || read64(old, manager::COMPLETED) != submitted_before
    {
        return Err(ComputeEncodeError::Descriptor);
    }
    w[compute::DESCRIPTOR..compute::DESCRIPTOR + compute::DESCRIPTOR_SIZE].copy_from_slice(old);
    put64(w, compute::DESCRIPTOR + manager::WORK, r.work);
    put32(w, compute::DESCRIPTOR + manager::INDEX, r.uma_slot);
    Ok(submitted_before + 1)
}

fn timestamp(q: &mut [u8], off: usize, end: bool, r: &ComputeRefs, requested: bool) {
    put32(q, off, if end { 3 } else { 0x80000003 });
    put64(q, off + 4, r.work + 0x82c);
    put64(q, off + 0xc, r.work + 0x834);
    put64(q, off + 0x14, r.work + if end { 0x83c } else { 0x834 });
    put64(q, off + 0x1c, r.qinfo);
    put64(q, off + 0x24, if requested { r.work + 0x844 } else { 0 });
    put64(q, off + 0x2c, r.work + 0x89c);
    put64(q, off + 0x34, r.work + 0x7dc);
    put64(q, off + 0x44, 1);
}

/// Builds detached work and microsequence bytes. The explicit event/stamp/slot chronology in
/// `r` is authoritative; `n` is only the shared engine's software ordinal. The zeroed sequence
/// is mirrored at work+0x900; its live pointer is `r.seq`, which may name that embedded copy or
/// a separate retained object. Absolute references into the sequence use `r.seq` in both cases.
#[allow(clippy::too_many_arguments)]
pub(crate) fn encode_compute(
    r: &ComputeRefs,
    c: &uapi::drm_asahi_cmd_compute,
    att: &[uapi::drm_asahi_attachment],
    ts_va: &[u64; 2],
    ctx: u32,
    _n: u64,
    fault_mask: u32,
    w: &mut [u8; compute::WORK_SIZE],
    q: &mut [u8; compute::SEQ_SIZE],
) -> Result<(), ComputeEncodeError> {
    if ctx == 0 || ctx >= 64 {
        return Err(ComputeEncodeError::Context);
    }
    if r.binding_generation == 0
        || r.uma_slot == 0
        || r.uma_slot > 32
        || r.target_stamp & 0xff != 0
        || r.submitted == 0
    {
        return Err(ComputeEncodeError::Chronology);
    }
    if !fw(r.work, compute::WORK_SIZE as u64)
        || !fw(r.seq, compute::SEQ_SIZE as u64)
        || !fw(r.qinfo, 0x24c0)
        || !fw(r.stats_fw, 0xe10)
        || !fw(r.auxiliary_fw, compute::AUX_SIZE as u64)
        || !fw(r.counter_fw, 8)
        || !fw(r.notifier, 0x118)
        || !fw(r.user_stamp, 8)
        || !fw(r.fw_stamp, 8)
        || !fw(r.its, 16)
        || (r.record_fw != 0 && !fw(r.record_fw, 0x118))
        || !gpu(r.work_gva, 0x4000, 0x4000)
        || !gpu(r.preempt_gva, compute::PREEMPT_SIZE as u64, 8)
        || !gpu(r.auxiliary_gva, compute::AUX_SIZE as u64, 0x4000)
        || !gpu(
            r.context_store_gva,
            compute::CONTEXT_STORE_SIZE as u64,
            0x4000,
        )
        || !gpu(r.pool_state_gva, compute::POOL_STATE_SIZE as u64, 0x4000)
        || !gpu(r.page_list_gva, compute::PAGE_LIST_SIZE as u64, 0x4000)
        || (r.usc_base != 0 && !gpu(r.usc_base, 0x100000000, 0x100000000))
    {
        return Err(ComputeEncodeError::Address);
    }
    let (base, end) = (c.cdm_ctrl_stream_base, c.cdm_ctrl_stream_end);
    if base & 3 != 0
        || end & 3 != 0
        || end <= base
        || end - base < 4
        || end - base > 1 << 31
        || !gpu(base, end - base, 4)
    {
        return Err(ComputeEncodeError::Command);
    }
    if att.len() > compute::MAX_ATTACHMENTS {
        return Err(ComputeEncodeError::Attachment);
    }
    for a in att {
        let end = a
            .pointer
            .checked_add(a.size)
            .and_then(|e| e.checked_add(127))
            .ok_or(ComputeEncodeError::Attachment)?
            & !127;
        let start = a.pointer & !127;
        if a.size == 0
            || end <= start
            || (end - start) / 128 > u64::from(u32::MAX)
            || !gpu(start, end - start, 128)
        {
            return Err(ComputeEncodeError::Attachment);
        }
    }
    // Timestamp aliases are independent firmware-write mappings of held user BO slices.
    for &va in ts_va {
        if va != 0 && !range(va, 8, 8, FW_END, 0xfffffc3000000000) {
            return Err(ComputeEncodeError::Address);
        }
    }
    let pre = r.preempt_gva;
    let values = [
        pre,
        base,
        pre + 0x1480,
        pre + 0x1488,
        pre + 0x1490,
        pre + 0x1498,
        0x154024201,
        0x10c08860,
        0x1c,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        8,
        0xff0000,
        0x13200400020,
        0x200000001,
        1,
        1,
        1,
    ];
    w.fill(0);
    q.fill(0);
    for (i, (&sel, &value)) in compute::SEL.iter().zip(values.iter()).enumerate() {
        put32(w, compute::REGS + i * 12, sel);
        put64(w, compute::REGS + i * 12 + 4, value);
    }
    let count = if r.usc_base == 0 { 24 } else { 25 };
    if r.usc_base != 0 {
        put32(w, compute::REGS + 24 * 12, 0x10071);
        put64(w, compute::REGS + 24 * 12 + 4, r.usc_base);
    }
    put32(w, 0, 3);
    put32(w, 0xc, ctx);
    put64(
        w,
        0x10,
        if r.record_fw == 0 {
            r.notifier
        } else {
            r.record_fw
        },
    );
    put64(w, 0x720, r.work_gva + 0x20);
    put16(w, 0x728, count);
    put16(w, 0x72a, count * 12);
    put64(w, 0x760, r.seq);
    put32(w, 0x768, compute::SEQ_SIZE as u32);
    put64(w, 0x798, pre);
    put64(w, 0x7a0, end - 4);
    put64(w, 0x7c8, 0x154024201);
    put32(w, 0x7e8, u32::MAX);
    put64(w, 0x800, r.user_stamp);
    put64(w, 0x808, r.fw_stamp);
    put32(w, 0x810, r.target_stamp);
    put32(w, 0x814, compute::EVENT);
    put32(w, 0x81c, 1);
    put32(w, 0x818, r.work_818);
    put64(w, 0x820, r.command_id);
    put64(w, 0x834, r.its);
    put64(w, 0x83c, r.its + 8);
    put64(w, 0x844, ts_va[0]);
    put64(w, 0x84c, ts_va[1]);
    put64(w, 0x86a, r.work + compute::DESCRIPTOR as u64);
    put8(w, 0x872, 1);
    put64(w, 0x873, 0x1588000);
    put64(w, 0x87b, 0xef8000);
    put64(w, 0x883, r.auxiliary_fw);
    put8(w, 0x88b, r.binding_generation);

    put32(q, 0, 0xb);
    put64(q, 0x14, r.work + 0x20);
    put64(q, 0x1c, r.stats_fw);
    put64(q, 0x24, r.qinfo);
    put32(q, 0x2c, ctx);
    put32(q, 0x30, u32::from(fault_mask != 0));
    put32(q, 0x38, r.compute_index);
    put32(q, 0x3c, r.work_818);
    put64(q, 0x44, r.work + 0x76c);
    put32(q, 0x50, 1);
    for (i, a) in att.iter().enumerate() {
        let start = a.pointer & !127;
        let end = (a.pointer + a.size + 127) & !127;
        let off = compute::ATTACHMENTS + i * compute::ATTACHMENT_STRIDE;
        put64(q, off, start);
        put32(q, off + 8, ((end - start) / 128) as u32);
        put16(q, off + 0xc, 0x18);
        put16(q, off + 0xe, 1);
    }
    put32(q, 0x158, att.len() as u32);
    put64(q, 0x160, r.work + compute::DESCRIPTOR as u64);
    put64(q, 0x178, r.auxiliary_gva);
    put64(q, 0x188, r.context_store_gva);
    put64(q, 0x190, r.context_store_gva + 0xf400);
    put64(q, 0x198, r.context_store_gva + 0x1e800);
    put64(q, 0x1a0, r.context_store_gva + 0x1f000);
    put64(q, 0x1a8, r.work + 0x8ac);
    put32(q, 0x1b8, compute::EVENT);
    let requested = ts_va[0] != 0 || ts_va[1] != 0;
    timestamp(q, compute::TS_START, false, r, requested);
    put32(q, 0x208, 1);
    timestamp(q, compute::TS_END, true, r, requested);
    let f = compute::FINALIZE;
    put32(q, f, 0xc);
    put64(q, f + 4, r.stats_fw);
    put64(q, f + 0xc, r.qinfo);
    put32(q, f + 0x14, ctx);
    put64(q, f + 0x18, r.work + 0x76c);
    put32(q, f + 0x24, 1);
    put64(q, f + 0x2c, r.fw_stamp);
    put32(q, f + 0x34, r.target_stamp);
    put64(q, f + 0x5c, r.seq + 0x160);
    put32(q, f + 0x64, 0u32.wrapping_sub(f as u32));
    put8(q, f + 0x68, u8::from(!att.is_empty()));
    put64(q, f + 0x69, r.work + 0x8ac);
    put64(q, f + 0x71, r.work + 0x88c);
    put32(q, compute::END, 0x40000002);
    w[compute::SEQ_EMBEDDED..compute::SEQ_EMBEDDED + compute::SEQ_SIZE].copy_from_slice(q);

    let no = compute::NOTIFIER;
    put64(w, no, r.work + 0xf00);
    put32(w, no + 0x10, 0x50);
    put32(w, no + 0x24, ctx);
    put64(w, no + 0xa8, u64::MAX);
    put32(w, no + 0x114, 1);
    let d = compute::DESCRIPTOR;
    put64(w, d + manager::WORK, r.work);
    put32(w, d + manager::INDEX, r.uma_slot);
    put32(w, d + manager::ONE, 2);
    put64(w, d + manager::FREELIST, r.pool_state_gva);
    put32(w, d + manager::UNK_1C, manager::UNK_1C_VALUE);
    put32(w, d + manager::SEEDS_A, 8192);
    put32(w, d + manager::SEEDS_B, 8192);
    put64(w, d + manager::POOL_TABLE, r.page_list_gva);
    put32(w, d + 0x40, 4);
    put32(w, d + 0x48, 128);
    put64(w, compute::DESCRIPTOR_COUNTER, r.counter_fw);
    // Native M+0x5c distinguishes shared compute from per-workqueue-private pools.
    // Retain the existing value for these persistent units reused across queues;
    // their custom pool topology does not establish native pool-role equivalence.
    // preserve_compute_descriptor retains ASC bookkeeping on every later use.
    put32(w, d + manager::COMPUTE_EXCLUSION, 1);
    put32(w, d + 0x84, 1);
    put32(w, d + 0x94, 2);
    put32(w, 0xee0, u32::MAX);
    put32(w, 0xee4, u32::MAX);
    put64(w, 0xf00, 1);
    Ok(())
}
