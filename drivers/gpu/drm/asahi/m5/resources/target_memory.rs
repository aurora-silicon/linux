// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Driver-owned render targets in the actual submitting VM.

use super::{graphics_memory::M5GraphicsMemory, MappedObject};
use crate::{driver::AsahiDrmDevice, m5::render_desc::G17pRenderParameters, mmu, m5::vm_layout,
    m5::render_target::{self, M5RenderTargetLayout, PAGE_BYTES, AUX_BYTES}};
use kernel::prelude::*;

pub(super) struct M5TargetMemory {
    target: MappedObject,
    tpc: MappedObject,
    deflake: MappedObject,
    ta_status: MappedObject,
    fragment_status: MappedObject,
    auxiliary: MappedObject,
    layout: M5RenderTargetLayout,
}

impl M5TargetMemory {
    pub(super) fn new(
        dev: &AsahiDrmDevice, memory: &M5GraphicsMemory, p: &G17pRenderParameters,
        clusters: u32,
    ) -> Result<Self> {
        let layout = M5RenderTargetLayout::new(p.width, p.height, p.layers,
            p.utile_width, p.utile_height, clusters).ok_or(EINVAL)?;
        let allocate = |size| memory.allocate_compact(
            dev, size, 0x8000, mmu::PROT_GPU_SHARED_RW,
        );
        let mut owner = Self {
            target: allocate(layout.target_bytes)?, tpc: allocate(layout.tpc_bytes)?,
            deflake: allocate(PAGE_BYTES)?, ta_status: allocate(PAGE_BYTES)?,
            fragment_status: allocate(PAGE_BYTES)?, auxiliary: allocate(AUX_BYTES)?, layout,
        };
        owner.deflake.with_bytes_mut(|bytes| {
            render_target::initialize_deflake(bytes, p.width, p.height).ok_or(EINVAL)
        })?;
        owner.validate(memory.vm())?;
        dev_dbg!(dev.as_ref(),
            "M5_RENDER_TARGET target={:#x}/{:#x} heapmeta={:#x} tpc={:#x}/{:#x} deflake={:#x} status={:x?} auxiliary={:#x}; VM-owned\n",
            owner.target.iova(), layout.target_bytes, owner.target.iova() + layout.heapmeta_offset as u64,
            owner.tpc.iova(), layout.tpc_bytes, owner.deflake.iova(),
            [owner.ta_status.iova(), owner.fragment_status.iova()], owner.auxiliary.iova());
        Ok(owner)
    }

    pub(super) fn apply(&self, p: &mut G17pRenderParameters) {
        p.context_base = vm_layout::DRIVER_BASE;
        p.tilemap = self.target.iova();
        p.heapmeta = self.target.iova() + self.layout.heapmeta_offset as u64;
        p.tpc = self.tpc.iova();
        p.deflake_3 = self.deflake.iova();
        p.deflake_2 = self.deflake.iova() + 0x40;
        p.deflake_1 = self.deflake.iova() + 0x340;
        p.ta_status = self.ta_status.iova();
        p.fragment_status = self.fragment_status.iova();
        p.aux_fb = self.auxiliary.iova();
    }

    fn validate(&self, vm: &mmu::Vm) -> Result {
        for object in [&self.target, &self.tpc, &self.deflake, &self.ta_status,
                       &self.fragment_status, &self.auxiliary] {
            let start = object.iova();
            let end = start.checked_add(object.logical_size as u64).ok_or(EOVERFLOW)?;
            if start < vm_layout::DRIVER_BASE || end > vm_layout::COMPACT_END
                || start & 0x7fff != 0 { return Err(ERANGE); }
            for offset in (0..object.logical_size).step_by(PAGE_BYTES) {
                vm.translate_iova(start + offset as u64)?;
            }
            for page in 0..2 {
                if vm.translate_iova(end + page * PAGE_BYTES as u64).is_ok() { return Err(EFAULT); }
            }
        }
        Ok(())
    }
}
