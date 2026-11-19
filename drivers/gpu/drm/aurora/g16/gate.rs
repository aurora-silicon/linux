// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Best-effort stream admission checks for userspace encoders.
//!
//! GPU-writable command memory can change after validation, and shader/indirect execution
//! remains opaque. These checks diagnose malformed streams and bound obvious walks; they
//! are not an isolation/security boundary or proof that a command will terminate.
//!
//! The G16G VDM root barrier and AGX3 CDM records follow Mesa's qualified
//! `agx_vdm_barrier_core.c`, `agx3_cmdstream.h` and `agx3_cdm_decode_core.c`.
//! Program addresses use the 48-bit AGX3 split (word1[31:22], word2[31:0]);
//! do not reuse the legacy G15 gate's repeated-address checks. The gate
//! refuses unfinished walks, cycles and unsupported call/return links. Large
//! streams are walked to completion with scheduling and signal checks, rather
//! than imposing a command-count limit on otherwise valid userspace streams.

use core::sync::atomic::{AtomicU32, Ordering};
use kernel::prelude::*;

use super::{command::cover, command::USC_WINDOW, params};
use crate::mmu;

const VDM_ROOT: u32 = 0x21000960;
const CDM_TERMINATE: u32 = 0x40000000;
const CDM_BARRIERS: [u32; 4] = [0x60000160, 0x600215fe, 0x60001560, 0x600201fe];
const CDM_LINK: u32 = 0x20000000;
/// Scheduling interval, not a limit on admitted stream length.
const CDM_RESCHED_RECORDS: u32 = 1024;
/// Rejections logged per boot.
const MAX_REPORTS: u32 = 32;

static REPORTS: AtomicU32 = AtomicU32::new(0);

/// Logs a refused record and refuses it.
fn reject(what: &str, va: u64, w: &[u32]) -> Error {
    if REPORTS.fetch_add(1, Ordering::Relaxed) < MAX_REPORTS {
        let word = |i: usize| w.get(i).copied().unwrap_or(0);
        pr_warn!(
            "G16: rejecting {} record at {:#x}: {:08x} {:08x} {:08x} {:08x}\n",
            what,
            va,
            word(0),
            word(1),
            word(2),
            word(3)
        );
    }
    EINVAL
}

/// Up to 256 stream bytes read at once.
struct Window {
    va: u64,
    len: usize,
    bytes: [u8; 256],
}

impl Window {
    /// The `count` words at `va`, reading whole spans of up to 256 bytes (not past the page
    /// unless the record crosses it). Unreadable memory: EFAULT.
    fn words(&mut self, vm: &mmu::Vm, va: u64, count: usize) -> Result<[u32; 10]> {
        let n = count.checked_mul(4).filter(|&n| n <= 40).ok_or(EINVAL)?;
        cover(vm, va, va.checked_add(n as u64).ok_or(EINVAL)?, true, false)?;
        let cached = self.len != 0
            && va >= self.va
            && va
                .checked_add(n as u64)
                .is_some_and(|end| end <= self.va + self.len as u64);
        if !cached {
            let in_page = mmu::UAT_PGSZ - (va as usize & mmu::UAT_PGMSK);
            let len = n.max(self.bytes.len().min(in_page));
            self.len = 0;
            vm.read_user(va, &mut self.bytes[..len])
                .map_err(|_| EFAULT)?;
            self.va = va;
            self.len = len;
        }
        let start = (va - self.va) as usize;
        let mut words = [0u32; 10];
        for (index, word) in words.iter_mut().take(count).enumerate() {
            let at = start + index * 4;
            let mut le = [0u8; 4];
            le.copy_from_slice(&self.bytes[at..at + 4]);
            *word = u32::from_le_bytes(le);
        }
        Ok(words)
    }
}

/// A launch's program: inside the queue's 4 GiB USC window, 64-aligned, first block readable.
/// `checked` caches the last proven program page.
fn program(vm: &mmu::Vm, usc: u64, address: u64, checked: &mut Option<u64>) -> Result {
    if address < usc || address - usc >= USC_WINDOW || address & 63 != 0 {
        return Err(EINVAL);
    }
    // 64-aligned: the block never crosses its page.
    let page = address & !(mmu::UAT_PGMSK as u64);
    if *checked == Some(page) {
        return Ok(());
    }
    cover(vm, address, address + 64, true, false)?;
    *checked = Some(page);
    Ok(())
}

/// VDM admission: a normal root stream opens with exactly the G16 barrier.
pub(crate) fn vdm(vm: &mmu::Vm, va: u64) -> Result {
    if !params::stream_gate() {
        return Ok(());
    }
    let mut bytes = [0u8; 4];
    vm.read_user(va, &mut bytes).map_err(|_| EFAULT)?;
    let word = u32::from_le_bytes(bytes);
    if word != VDM_ROOT {
        return Err(reject("VDM root", va, &[word]));
    }
    Ok(())
}

/// CDM admission: walks the root and no-return linked streams to TERMINATE.
/// All requested bytes are proven readable; the root cannot end inside a record.
/// Mode 0 has six inline grid words, mode 1 three inline local-size words and
/// an indirect global grid, mode 2 an indirect six-word grid. The remaining
/// launch-template payload words stay opaque, as in Mesa's raw AGX3 decoder.
pub(crate) fn cdm(vm: &mmu::Vm, usc: u64, base: u64, end: u64) -> Result {
    if !params::stream_gate() {
        return Ok(());
    }
    if base & 3 != 0 || end <= base || end & 3 != 0 {
        return Err(EINVAL);
    }
    let mut window = Window {
        va: 0,
        len: 0,
        bytes: [0; 256],
    };
    let mut checked = None;
    let mut va = base;
    let mut linked = false;
    // Brent's cycle detection needs constant memory and checks each record only
    // once. A no-return stream is deterministic: revisiting a record is a loop.
    // Unlike a record/link budget, this admits arbitrarily long acyclic chains.
    let mut checkpoint = base;
    let mut distance = 0u64;
    let mut power = 1u64;
    let mut first = true;
    let mut resched = 0;
    loop {
        if first {
            first = false;
        } else {
            distance += 1;
            if va == checkpoint {
                return Err(reject("CDM cycle", va, &[]));
            }
            if distance == power {
                checkpoint = va;
                distance = 0;
                power = power.saturating_mul(2);
            }
        }
        if resched == 0 {
            // No VM lock is held between reads. Keep a long userspace stream
            // preemptible and allow its submitting task to be interrupted.
            // might_sleep() includes might_resched() (cond_resched on voluntary kernels).
            // It is a real scheduling point, not only a debug assertion.
            kernel::task::might_sleep();
            if kernel::current!().signal_pending() {
                return Err(ERESTARTSYS);
            }
            resched = CDM_RESCHED_RECORDS;
        }
        resched -= 1;
        let root_bytes = |va: u64, bytes: u64| -> Result {
            if !linked && (va < base || va.checked_add(bytes).is_none_or(|at| at > end)) {
                return Err(EINVAL);
            }
            Ok(())
        };
        root_bytes(va, 4)?;
        let w = window.words(vm, va, 1)?;
        if w[0] == CDM_TERMINATE {
            return Ok(());
        }
        // Canonical G16G barriers, including its wide-visibility variants.
        // Virtual substream barriers carry a one-biased index in bits 21..28.
        let substream = w[0] & 0x1fe00000;
        if CDM_BARRIERS.contains(&w[0])
            || (substream != 0 && matches!(w[0] & !0x1fe00000, 0x60000160 | 0x60001560))
        {
            va = va.checked_add(4).ok_or(EINVAL)?;
            continue;
        }
        // Link padding itself is one four-byte record in the AGX3 encoder.
        if w[0] & !7 == 0xa0000000 {
            va = va.checked_add(4).ok_or(EINVAL)?;
            continue;
        }
        if w[0] & 0xffff0000 == CDM_LINK {
            root_bytes(va, 8)?;
            let w = window.words(vm, va, 2)?;
            let target = (u64::from(w[0] & 0xffff) << 32) | u64::from(w[1]);
            if target & 3 != 0 || target == va {
                return Err(reject("CDM link", va, &w));
            }
            va = target;
            linked = true;
            continue;
        }
        if w[0] >> 29 != 0 {
            return Err(reject("CDM", va, &w));
        }
        let mode = (w[0] >> 27) & 3;
        let words = match mode {
            0 => 10,
            1 => 9,
            2 => 6,
            _ => return Err(reject("CDM launch mode", va, &w)),
        };
        root_bytes(va, (words * 4) as u64)?;
        let w = window.words(vm, va, words)?;
        let address = (u64::from(w[1] >> 22) << 38) | (u64::from(w[2]) << 6);
        match program(vm, usc, address, &mut checked) {
            Err(error) if error == EINVAL => return Err(reject("CDM program", va, &w)),
            result => result?,
        }
        let sizes = if mode == 0 {
            &w[7..10]
        } else if mode == 1 {
            &w[6..9]
        } else {
            &[]
        };
        if sizes.iter().any(|&size| size > 0x7ff) {
            return Err(reject("CDM local size", va, &w));
        }
        if mode != 0 {
            let indirect = (u64::from(w[4] & 0xffff) << 32) | u64::from(w[5]);
            let bytes = if mode == 1 { 12 } else { 24 };
            if indirect & 3 != 0 {
                return Err(reject("CDM indirect parameters", va, &w));
            }
            cover(
                vm,
                indirect,
                indirect.checked_add(bytes).ok_or(EINVAL)?,
                true,
                false,
            )?;
        }
        va = va.checked_add((words * 4) as u64).ok_or(EINVAL)?;
    }
}
