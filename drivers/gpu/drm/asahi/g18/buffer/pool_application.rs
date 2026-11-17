// SPDX-License-Identifier: GPL-2.0-only OR MIT
use super::*;
impl G18UmaPoolMemory {
    pub(crate) fn verify_application_descriptor(&mut self, table: &mut MappedObject,
        device_config: [u64;4], first: bool) -> Result<[u64;16]> {
        if first {return self.verify_render_descriptor(table,device_config);}
        if !self.render || !self.descriptor_loaded {return Err(EINVAL);}
        let state: [u8;g18_uma::G18_UMA_STATE_BYTES]=self.state.with_bytes_mut(|out|
            out[..g18_uma::G18_UMA_STATE_BYTES].try_into().map_err(|_|EINVAL))?;
        let expected=g18_uma::pack_g18_uma_descriptor(&state,device_config);
        let mut actual=[0;16];
        for (i,v) in actual.iter_mut().enumerate() {*v=table.read_u64(table.iova()+i as u64*8)?;}
        // Directory address/stride, device masks, modes, block-list address
        // and block geometry stay fixed. Allocation cursors/counters remain
        // hardware-owned; never restore the initial row over their progress.
        for i in [0,3,4,5,6,10] {
            if actual[i]!=expected[i] {return Err(EIO);}
        }
        if actual[11]>u64::from(crate::g18::uapi::render_policy::UMA_BLOCKS) {return Err(EIO);}
        Ok(actual)
    }

    pub(crate) fn application_aliases(&mut self, uat: &mmu::Uat, vm: &mmu::Vm)
        -> Result<KVec<mmu::KernelMapping>> {
        if !self.render || self.scratch_plan.is_none() {return Err(EINVAL);}
        let mut aliases=KVec::new();
        for (index,page) in self.directory.iter_mut().enumerate() {
            let address=DIRECTORY_BASE+self.layout.page_offset(index as u32).ok_or(EINVAL)?;
            aliases.push(page.map_alias_at(vm,address,mmu::PROT_GPU_SHARED_RW)?,GFP_KERNEL)?;
        }
        aliases.push(self.block_list.map_alias_at(vm,BLOCK_LIST_BASE,mmu::PROT_GPU_SHARED_RW)?,GFP_KERNEL)?;
        for (index,block) in self.scratch.iter_mut().enumerate() {
            aliases.push(block.map_alias_at(vm,BLOCK_DATA_BASE+index as u64*BLOCK_DATA_STRIDE,
                mmu::PROT_GPU_SHARED_RW)?,GFP_KERNEL)?;
        }
        self.audit_render_mappings(uat,vm)?;Ok(aliases)
    }
}
