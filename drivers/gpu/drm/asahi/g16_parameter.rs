// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Retained physical parameter-buffer storage, separate from render scenes.
//! A shared pool is initialized once per admitted packet. Every scene retains
//! its owner; no pool backing or allocator state is rebuilt while referenced.
use kernel::{prelude::*, sync::{Arc, Mutex}};
use crate::{driver, mmu, g16_memory::Buffer, g16_render::{Geometry, Utile},
    g16_render_command as wire, g16_render_state as state, g16_uapi::UapiRenderCommand};

const BLOCK_BYTES: usize = 4 * 32768;
pub(crate) type Pool = Arc<Mutex<Parameters>>;
pub(crate) struct Parameters {
    pub(crate) manager: Buffer,
    pub(crate) ring: Buffer,
    pages: Buffer,
    blocks: Buffer,
    memory: Buffer,
    counter: Buffer,
    pub(crate) id: u32,
    pub(crate) shared: bool,
    claimed: bool,
    layout: wire::ParameterLayout,
}

pub(crate) fn geometry(r: UapiRenderCommand) -> Result<Geometry> {
    let utile = |n| match n {16=>Ok(Utile::Pixels16),32=>Ok(Utile::Pixels32),_=>Err(EINVAL)};
    let mut g = Geometry::new_layered(r.width as u32, r.height as u32,
        utile(r.utile_width)?, utile(r.utile_height)?, 1, r.layers).map_err(|_| EINVAL)?;
    g.set_samples(r.samples).map_err(|_| EINVAL)?;
    Ok(g)
}
pub(crate) fn bytes(g: &Geometry) -> Result<usize> {
    Ok(g.parameter_bytes())
}
impl Parameters {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        bytes: usize, id: u32, shared: bool) -> Result<Pool> {
        if bytes < BLOCK_BYTES || bytes % BLOCK_BYTES != 0 ||
            id >= crate::g16_runtime::POOL_SLOTS as u32 { return Err(EINVAL); }
        let pages = bytes / 32768;
        let blocks = bytes / BLOCK_BYTES;
        let capacity = (blocks+1).next_power_of_two();
        let mut pool = Self {
            manager: Buffer::new(dev, uat.kernel_vm(), wire::parameter::SIZE)?,
            ring: Buffer::new(dev, uat.kernel_vm(), 8)?,
            pages: Buffer::new_gpu(dev, uat.kernel_vm(), vm, pages*4)?,
            blocks: Buffer::new(dev, uat.kernel_vm(), capacity*8)?,
            memory: Buffer::new_gpu_aligned(dev, uat.kernel_vm(), vm, bytes, BLOCK_BYTES as u64)?,
            counter: Buffer::new(dev, uat.kernel_vm(), 0x40)?, id, shared, claimed:false,
            layout: wire::ParameterLayout::for_chip(crate::g16_board::get()?.chip_id),
        };
        pool.rebuild(dev, uat, vm, bytes)?;
        Arc::pin_init(kernel::new_mutex!(pool), GFP_KERNEL)
    }
    pub(crate) fn rebuild(&mut self, dev: &driver::AsahiDevice, uat: &mmu::Uat,
        vm: &mmu::Vm, bytes: usize) -> Result {
        if self.shared && self.claimed { return Err(EBUSY); }
        let page_count = bytes / 32768;
        let block_count = bytes / BLOCK_BYTES;
        let capacity = (block_count+1).next_power_of_two();
        if self.pages.size() < page_count*4 {
            self.pages = Buffer::new_gpu(dev, uat.kernel_vm(), vm, page_count*4)?;
        }
        if self.blocks.size() < capacity*8 {
            self.blocks = Buffer::new(dev, uat.kernel_vm(), capacity*8)?;
        }
        if self.memory.size() < bytes {
            self.memory = Buffer::new_gpu_aligned(dev, uat.kernel_vm(), vm, bytes, BLOCK_BYTES as u64)?;
        }
        for buffer in [&mut self.manager, &mut self.ring, &mut self.pages,
            &mut self.blocks, &mut self.counter] { buffer.clear()?; }
        let base = state::compact(self.memory.gpu_va()?).map_err(|_| EINVAL)?;
        let mut pages = KVec::new();
        for i in 0..page_count {
            let page: u32 = ((base+i as u64*32768)>>15).try_into()?;
            pages.extend_from_slice(&page.to_le_bytes(), GFP_KERNEL)?;
        }
        self.pages.write(0, &pages)?;
        let mut blocks = KVec::new();
        for i in 0..block_count {
            let block: u32 = ((base+i as u64*BLOCK_BYTES as u64)>>15).try_into()?;
            blocks.extend_from_slice(&block.to_le_bytes(), GFP_KERNEL)?;
            blocks.extend_from_slice(&[0; 4], GFP_KERNEL)?;
        }
        self.blocks.write(0, &blocks)?;
        self.ring.u32(0, block_count.try_into()?)?;
        self.ring.u32(4, block_count.try_into()?)?;
        let mut manager = [0;wire::parameter::SIZE];
        wire::parameter_manager_for_layout(&mut manager, wire::ParameterManager {
            pages_fw:self.pages.va(), pages_gpu:self.pages.gpu_va()?,
            blocks:self.blocks.va(), ring:self.ring.va(), counter:self.counter.va(),
            discard:0, list_bytes:(page_count*4).try_into()?, pages:page_count.try_into()?,
            block_capacity:capacity.try_into()?, write:block_count.try_into()?, read:0,
            min_pages:page_count.try_into()?, max_pages:page_count.try_into()?, id:self.id,
        }, self.layout).map_err(|_| EINVAL)?;
        self.manager.write(0, &manager)?;
        self.claimed = false;
        Ok(())
    }
    pub(crate) fn last_page(&mut self) -> Result<u32> {
        self.manager.read_u32(self.layout.last_page())
    }
    pub(crate) fn pages(&self) -> usize { self.memory.size() / 32768 }
    pub(crate) fn resident_bytes(&self) -> usize {
        [&self.manager, &self.ring, &self.pages, &self.blocks, &self.memory, &self.counter]
            .iter().map(|b| crate::util::align(b.size(), crate::mmu::UAT_PGSZ)).sum()
    }
    pub(crate) fn claim_init(&mut self) -> bool {
        let first = !self.claimed;
        self.claimed = true;
        first
    }
}
