// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Retained private G18 compute fixture in its own, unpublished process VM.
//! Context-root publication, command aliases, device policy and the queue kick
//! are separate requirements. Loading this object never submits work.

#![allow(dead_code)]

use crate::g18::buffer::object::MappedObject;
use crate::{driver::AsahiDrmDevice, g18::fw::compute::*, g18::fixture::compute_payload as payload,
    g18::fixture::compute_triangle::{TrianglePatch, TriangleResult, PATCH_COUNT},
    g18::fw::compute_state::{G18ComputeState, G18_COMPUTE_STATE_BYTES}, mmu};
use core::sync::atomic::{fence, Ordering};
use kernel::{firmware::Firmware, prelude::*};

/// The verified CDM control stream (register 0x1a420) is a 40-byte direct
/// launch, this barrier and this terminate. Command +0xee0 (`d4e0`, the
/// G17/M5 `cdm_terminator`) is the terminate word's address. Block type is
/// bits 31:29, as Mesa agx3_cmdstream.h decodes this capture.
const CDM_LAUNCH_BYTES: usize = 40;
const CDM_BARRIER: u32 = 0x6000_0160;
const CDM_TERMINATE: u32 = 0x4000_0000;

fn stream_words(bytes: &[u8]) -> [u32; 12] {
    core::array::from_fn(|i| u32::from_le_bytes(bytes[4 * i..4 * i + 4].try_into().unwrap()))
}

pub(crate) struct G18ComputePayloadMemory {
    // Mappings and their GEM storage are released before the VM root owner.
    pages: KVec<MappedObject>,
    vm: mmu::Vm,
    state: [u8; G18_COMPUTE_STATE_BYTES],
    output: usize,
    /// `g18_compute_variant=1` replacement for command +0xee0.
    cdm_terminator: Option<u64>,
}

impl G18ComputePayloadMemory {
    pub(crate) fn load(dev: &AsahiDrmDevice, uat: &mmu::Uat) -> Result<Self> {
        if !uat.is_t8152_geometry() || mmu::UAT_PGSZ != payload::PAGE_BYTES {
            return Err(ENODEV);
        }
        let firmware = Firmware::request(c"asahi/g18-26a428-compute-add.bin", dev.as_ref().as_ref())?;
        let reference = payload::ComputePayload::parse(firmware.data()).map_err(|_| EINVAL)?;
        let vm = uat.new_t8152_compute_fixture_vm()?;
        let mut pages = KVec::new();
        let mut output = None;
        for index in 0..reference.page_count() {
            let input = reference.page(index).ok_or(EINVAL)?;
            let mut page = MappedObject::new_wc_at(dev, &vm, input.address,
                payload::PAGE_BYTES, mmu::PROT_GPU_SHARED_RW)?;
            page.with_bytes_mut(|bytes| {
                bytes.copy_from_slice(input.bytes);
                if input.address == payload::OUTPUT {
                    // Defense in depth: never inherit a capture's answer.
                    for word in bytes.chunks_exact_mut(4) {
                        word.copy_from_slice(&payload::POISON.to_le_bytes());
                    }
                }
                Ok(())
            })?;
            vm.translate_iova(input.address)?;
            vm.translate_iova(input.address + payload::PAGE_BYTES as u64 - 1)?;
            if input.address == payload::OUTPUT { output = Some(index); }
            pages.push(page, GFP_KERNEL)?;
        }
        let state = reference.state_bytes().try_into().map_err(|_| EINVAL)?;
        let mut owner = Self { pages, vm, state, output: output.ok_or(EINVAL)?, cdm_terminator: None };
        let initial = owner.output_snapshot()?;
        if initial.checked != 4096 || initial.untouched != 4096 || initial.correct != 0 {
            return Err(EIO);
        }
        match crate::g18::params::g18_compute_variant.value() {
            0 => {}
            1 => owner.empty_cdm_stream(dev)?,
            2 => owner.triangle_shader(dev, payload::fnv64(&firmware.data()[payload::HEADER_BYTES..]))?,
            _ => return Err(EINVAL),
        }
        fence(Ordering::SeqCst);
        dev_info!(dev.as_ref(),
            "G18 compute payload: {} pages retained, all 4096 outputs poisoned; private VM roots unpublished\n",
            owner.pages.len());
        Ok(owner)
    }

    pub(crate) fn vm(&self) -> &mmu::Vm { &self.vm }

    pub(crate) fn scratch_plan(&self) -> Result<crate::g18::fw::uma::G18ComputeScratchPlan> {
        crate::g18::fw::uma::g18_addition_scratch(&self.state).ok_or(EINVAL)
    }

    pub(crate) fn apply_state(&self, c: &mut G18ComputeCommandInputs, r: &mut G18ComputeRegisterInputs) {
        G18ComputeState::parse(&self.state).unwrap().apply(c, r);
    }

    pub(crate) fn addition_inputs(&self)
        -> Result<(G18ComputeCommandInputs, G18ComputeRegisterInputs)> {
        let (mut command, registers) = payload::addition_inputs(&self.state).map_err(|_| EINVAL)?;
        if let Some(terminator) = self.cdm_terminator { command.d4e0 = terminator; }
        Ok((command, registers))
    }

    /// Stage8 bisection, `g18_compute_variant=1`: dispatch nothing. Only after
    /// the unchanged verified load, rewrite the stream as the capture's own
    /// barrier and terminate at +0/+4, zero +8..+0x2f (the old launch,
    /// barrier and terminate) and point command +0xee0 at the new terminate.
    /// Every other byte, page and register is unchanged; nothing is published.
    fn empty_cdm_stream(&mut self, dev: &AsahiDrmDevice) -> Result {
        let mut command = G18ComputeCommandInputs::default();
        let mut registers = G18ComputeRegisterInputs::default();
        self.apply_state(&mut command, &mut registers);
        // g18_compute.rs emits program[1] as register 0x1a420.
        let (stream, terminator) = (registers.program[1], command.d4e0);
        let end = CDM_LAUNCH_BYTES + 8;
        let page = self.pages.iter().position(|page| page.iova() == stream);
        let patched = match page {
            Some(index) if terminator == stream + end as u64 - 4 =>
                self.pages[index].with_bytes_mut(|bytes| {
                    let before = stream_words(bytes);
                    if before[0] >> 29 != 0 || before[10] != CDM_BARRIER
                        || before[11] != CDM_TERMINATE || bytes[end..].iter().any(|b| *b != 0) {
                        return Ok(None);
                    }
                    bytes[..end].fill(0);
                    bytes[..4].copy_from_slice(&CDM_BARRIER.to_le_bytes());
                    bytes[4..8].copy_from_slice(&CDM_TERMINATE.to_le_bytes());
                    Ok(Some((before, stream_words(bytes))))
                })?,
            _ => None,
        };
        let Some((before, after)) = patched else {
            dev_err!(dev.as_ref(),
                "G18_COMPUTE_VARIANT=1 refused: stream {:#x} terminator {:#x} is not the captured launch, barrier and terminate; nothing changed\n",
                stream, terminator);
            return Err(EINVAL);
        };
        self.cdm_terminator = Some(stream + 4);
        dev_info!(dev.as_ref(),
            "G18_COMPUTE_VARIANT=1 empty stream {:#x}: words {:08x?} (were {:08x?}); command +0xee0 d4e0 {:#x} -> {:#x}; registers and all other bytes unchanged\n",
            stream, after, before, terminator, stream + 4);
        Ok(())
    }

    /// Validate every original site before mutating any page. All pages and
    /// roots are still private; failure cannot publish a partial program.
    fn triangle_shader(&mut self, dev: &AsahiDrmDevice, base_fnv: u64) -> Result {
        let firmware = Firmware::request(c"asahi/g18-26a428-compute-triangle.bin", dev.as_ref().as_ref())?;
        let patch = TrianglePatch::parse(firmware.data(), base_fnv).ok_or(EINVAL)?;
        let mut indices = [0usize; PATCH_COUNT];
        for (i, index) in indices.iter_mut().enumerate() {
            let site = patch.site(i).ok_or(EINVAL)?;
            let base = site.address & !(payload::PAGE_BYTES as u64 - 1);
            let offset = (site.address - base) as usize;
            *index = self.pages.iter().position(|page| page.iova() == base).ok_or(EINVAL)?;
            self.pages[*index].with_bytes_mut(|bytes| {
                let before = bytes.get(offset..offset + site.bytes.len()).ok_or(EINVAL)?;
                if payload::fnv64(before) != site.before_fnv { return Err(EINVAL); }
                Ok(())
            })?;
        }
        for (i, index) in indices.into_iter().enumerate() {
            let site = patch.site(i).ok_or(EINVAL)?;
            let offset = (site.address & (payload::PAGE_BYTES as u64 - 1)) as usize;
            self.pages[index].with_bytes_mut(|bytes| {
                let target = bytes.get_mut(offset..offset + site.bytes.len()).ok_or(EINVAL)?;
                target.copy_from_slice(site.bytes);
                fence(Ordering::SeqCst);
                for (j, expected) in site.bytes.iter().enumerate() {
                    // SAFETY: j is within this initialized, CPU-mapped slice.
                    if unsafe { core::ptr::read_volatile(target.as_ptr().add(j)) } != *expected {
                        return Err(EIO);
                    }
                }
                Ok(())
            })?;
            dev_info!(dev.as_ref(), "G18_COMPUTE_VARIANT=2 patch {} address={:#x} bytes={} before_fnv={:#x} after_fnv={:#x}; VM unpublished\n",
                i, site.address, site.bytes.len(), site.before_fnv, payload::fnv64(site.bytes));
        }
        Ok(())
    }

    pub(crate) fn triangle_output_snapshot(&mut self) -> Result<TriangleResult> {
        fence(Ordering::SeqCst);
        self.pages[self.output].with_bytes_mut(|bytes| TriangleResult::inspect(bytes).ok_or(EINVAL))
    }

    /// Call only while the GPU is quiescent or after an independently verified
    /// completion fence. This read does not establish completion itself.
    pub(crate) fn output_snapshot(&mut self) -> Result<payload::ComputeResult> {
        fence(Ordering::SeqCst);
        self.pages[self.output].with_bytes_mut(|bytes|
            payload::ComputeResult::inspect(bytes).ok_or(EINVAL))
    }
}
