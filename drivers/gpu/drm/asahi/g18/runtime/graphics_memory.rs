// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Retained G18GFX01 v2 pages. Construction never publishes roots or work.
use crate::g18::buffer::object::MappedObject;
use crate::{driver::AsahiDrmDevice, g18::address_map as va, g18::fw::graphics_payload as payload, mmu};
use core::sync::atomic::{fence, Ordering};
use kernel::{firmware::Firmware, prelude::*};

pub(crate) struct G18GraphicsMemory {
    // Root lease retracts before aliases/backing can drop. The live session
    // retains the whole job even after faults; this order also protects errors
    // during construction before publication.
    context: mmu::G18ExecutionContext,
    aliases: KVec<mmu::KernelMapping>,
    vdm_alias: Option<(u64, u64)>,
    ppp_aliased: bool,
    pages: KVec<MappedObject>,
    capture: payload::Capture,
    body_digest: u64,
    command_digest: u64,
    command: KVec<u8>,
}

impl G18GraphicsMemory {
    pub(crate) fn load_payload(dev: &AsahiDrmDevice, uat: &mmu::Uat, profile: u32) -> Result<Self> {
        let name = match profile {
            1 => c"asahi/g18-26a428-color-a-v2.bin",
            2 => c"asahi/g18-26a428-color-b-v2.bin",
            3 => c"asahi/g18-26a428-gears-g1-v2.bin",
            4 => c"asahi/g18-26a428-fragment-witness-v2.bin",
            _ => return Err(EINVAL),
        };
        if !uat.is_t8152_geometry() || mmu::UAT_PGSZ != payload::PAGE_BYTES { return Err(ENODEV); }
        let firmware = match Firmware::request(name, dev.as_ref().as_ref()) {
            Ok(firmware) => firmware,
            // Stage 10 executes no capture bytes: every application command
            // is rebuilt from Mesa's parameters. It needs only the colour-a
            // page layout, which is mapped here with zeroed contents.
            Err(e) if e == ENOENT && profile == 1
                && crate::g18::params::g18_initdata_test.value() == 10 =>
                return Self::layout_only(dev, uat),
            Err(e) => return Err(e),
        };
        let input = payload::GraphicsPayload::parse(firmware.data()).map_err(|_| EINVAL)?;
        if input.capture() as u32 != profile { return Err(EINVAL); }
        let (context, aliases, pages) = Self::map_pages(dev, uat,
            (0..input.page_count()).map(|index| input.page(index).map(|page| (page.address, Some(page.bytes)))))?;
        let mut command = KVec::new();
        command.extend_from_slice(input.command(), GFP_KERNEL)?;
        let owner = Self { context, aliases, vdm_alias: None, ppp_aliased: false, pages, capture: input.capture(), command,
            body_digest: input.body_digest(), command_digest: payload::fnv64(input.command()) };
        fence(Ordering::SeqCst);
        dev_info!(dev.as_ref(), "G18_RENDER_PAYLOAD profile={} pages={} low_aliases={} body_fnv={:#018x} command_fnv={:#018x} context={} roots={:x?}; unpublished\n",
            profile, owner.pages.len(), owner.aliases.len(), owner.body_digest, owner.command_digest,
            owner.context_id(), owner.roots());
        Ok(owner)
    }

    /// The colour-a VM layout without its capture: zeroed pages, no command.
    fn layout_only(dev: &AsahiDrmDevice, uat: &mmu::Uat) -> Result<Self> {
        let (context, aliases, pages) = Self::map_pages(dev, uat,
            payload::color_a_pages().map(|(address, _)| Some((address, None))))?;
        let owner = Self { context, aliases, vdm_alias: None, ppp_aliased: false, pages,
            capture: payload::Capture::ColorA, command: KVec::new(), body_digest: 0, command_digest: 0 };
        fence(Ordering::SeqCst);
        dev_info!(dev.as_ref(), "G18_RENDER_PAYLOAD layout-only colour-a pages={} low_aliases={} context={} roots={:x?}; no capture, stage 10 encodes from Mesa only\n",
            owner.pages.len(), owner.aliases.len(), owner.context_id(), owner.roots());
        Ok(owner)
    }

    /// True when no capture was loaded: there is no H1 command to encode.
    pub(crate) fn layout_only_payload(&self) -> bool { self.command.is_empty() }

    fn map_pages<'a>(dev: &AsahiDrmDevice, uat: &mmu::Uat,
        sources: impl Iterator<Item = Option<(u64, Option<&'a [u8]>)>>)
        -> Result<(mmu::G18ExecutionContext, KVec<mmu::KernelMapping>, KVec<MappedObject>)> {
        let vm = uat.new_t8152_compute_fixture_vm()?;
        let context = uat.prepare_t8152_execution_context(&vm)?;
        let mut pages = KVec::new();
        let mut aliases = KVec::new();
        for source in sources {
            let (address, bytes) = source.ok_or(EINVAL)?;
            // Captured usage flags are not PTE permissions. These private
            // fixture mappings have an explicit diagnostic RW policy.
            let mut page = MappedObject::new_wc_at(dev, &vm, address,
                payload::PAGE_BYTES, mmu::PROT_GPU_SHARED_RW)?;
            page.with_bytes_mut(|out| {
                match bytes { Some(bytes) => out.copy_from_slice(bytes), None => out.fill(0) }
                if (payload::OUTPUT..payload::OUTPUT + payload::OUTPUT_BYTES).contains(&address) {
                    out.fill(0xa5);
                }
                Ok(())
            })?;
            if address < 0x78000 {
                aliases.push(page.map_alias_at(&vm, va::COMPACT_BIAS + address,
                    mmu::PROT_GPU_SHARED_RW)?, GFP_KERNEL)?;
                for delta in [0, va::PAGE - 1] {
                    if vm.translate_iova(address + delta)?
                        != vm.translate_iova(va::COMPACT_BIAS + address + delta)? {
                        return Err(EFAULT);
                    }
                }
            }
            vm.translate_iova(address)?;
            vm.translate_iova(address + va::PAGE - 1)?;
            pages.push(page, GFP_KERNEL)?;
        }
        Ok((context, aliases, pages))
    }

    pub(crate) fn vm(&self) -> &mmu::Vm { self.context.vm() }
    pub(crate) fn context_id(&self) -> u8 { self.context.context_id() }
    pub(crate) fn roots(&self) -> [u64; 2] { self.context.roots() }
    pub(crate) fn capture(&self) -> payload::Capture { self.capture }
    pub(crate) fn command(&self) -> &[u8] { &self.command }
    pub(crate) fn publish(&mut self) -> Result { self.context.publish() }
    /// Called only after both graphics queues have independently retired.
    pub(crate) fn rebind_retired(&mut self, vm: &mmu::Vm) -> Result { self.context.rebind(vm) }
    pub(crate) fn matches_vm(&self, vm: &mmu::Vm) -> bool { self.context.matches(vm) }
    pub(crate) fn terminated_digest(&mut self, address: u64) -> Result<u64> {
        let base=address & !(va::PAGE-1);
        let page=self.pages.iter_mut().find(|p|p.iova()==base).ok_or(EFAULT)?;
        page.with_bytes_mut(|bytes|Ok(payload::fnv64(bytes)))
    }

    pub(crate) fn audit(&self) -> Result<usize> {
        let mut checked = 0;
        for page in self.pages.iter() {
            for offset in [0, va::PAGE - 1] {
                let address = page.iova() + offset;
                self.vm().translate_iova(address)?;
                if page.iova() < 0x78000 && self.vm().translate_iova(address)?
                    != self.vm().translate_iova(address + va::COMPACT_BIAS)? { return Err(EFAULT); }
                checked += 1;
            }
        }
        if let Some((source, target)) = self.vdm_alias {
            for offset in [0, va::PAGE - 1] {
                if self.vm().translate_iova(source + offset)? !=
                    self.vm().translate_iova(target + offset)? { return Err(EFAULT); }
                checked += 1;
            }
        }
        if self.ppp_aliased {
            for source in va::GRAPHICS_HIGH_PPP_PAGES {
                let target = va::GRAPHICS_HIGH_PPP_BASE - 0x58000 + source;
                for offset in [0, va::PAGE - 1] {
                    if self.vm().translate_iova(source + offset)? !=
                        self.vm().translate_iova(target + offset)? { return Err(EFAULT); }
                    checked += 1;
                }
            }
        }
        Ok(checked)
    }

    /// Isolate PPP address encoding using the pinned native witness. Only the
    /// pointer bits in its 14 PPP records change; the pages themselves alias
    /// their original backing. No shader or PPP payload is rewritten.
    pub(crate) fn alias_ppp_for_address_probe(&mut self) -> Result {
        if self.context.is_current() || self.ppp_aliased { return Err(EBUSY); }
        if self.capture != payload::Capture::FragmentWitness ||
            self.vdm_alias != Some((0x18000, va::GRAPHICS_HIGH_VDM)) { return Err(EINVAL); }
        const RECORDS: [(usize, u32, u32); 14] = [
            (0x00, 0x58000, 6), (0x08, 0x58018, 33), (0x10, 0x5809c, 33),
            (0x34, 0x40, 5), (0x3c, 0x58120, 7), (0x44, 0x5813c, 6),
            (0x4c, 0x58154, 5), (0x54, 0x58168, 7), (0x5c, 0x58184, 5),
            (0x64, 0x6ac00, 10), (0x6c, 0x58198, 3), (0x74, 0x581a4, 2),
            (0x7c, 0x48000, 5), (0x84, 0x581ac, 3),
        ];
        self.pages.iter_mut().find(|p| p.iova() == 0x18000).ok_or(EFAULT)?
            .with_bytes_mut(|bytes| {
                if payload::fnv64(bytes) != 0x6f21_d211_cfaf_7af1 { return Err(EINVAL); }
                for (offset, pointer, words) in RECORDS {
                    if bytes[offset..offset + 4] != (words << 8).to_le_bytes() ||
                        bytes[offset + 4..offset + 8] != pointer.to_le_bytes() { return Err(EINVAL); }
                }
                Ok(())
            })?;
        let vm = self.context.vm();
        for source in va::GRAPHICS_HIGH_PPP_PAGES {
            let target = va::GRAPHICS_HIGH_PPP_BASE - 0x58000 + source;
            let page = self.pages.iter_mut().find(|p| p.iova() == source).ok_or(EFAULT)?;
            self.aliases.push(page.map_alias_at(vm, target, mmu::PROT_GPU_SHARED_RW)?, GFP_KERNEL)?;
        }
        self.ppp_aliased = true;
        self.audit()?;
        self.pages.iter_mut().find(|p| p.iova() == 0x18000).ok_or(EFAULT)?
            .with_bytes_mut(|bytes| {
                for (offset, pointer, words) in RECORDS {
                    let compact = va::GRAPHICS_HIGH_PPP_BASE - 0x58000 - va::COMPACT_BIAS + u64::from(pointer);
                    bytes[offset..offset + 4].copy_from_slice(&((words << 8) | (compact >> 32) as u32).to_le_bytes());
                    bytes[offset + 4..offset + 8].copy_from_slice(&(compact as u32).to_le_bytes());
                }
                Ok(())
            })?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Retain a second mapping of the unchanged native VDM page. This isolates
    /// the address used for fetching commands from their contents and backing.
    pub(crate) fn alias_vdm_for_address_probe(&mut self, address: u64) -> Result<u64> {
        if self.context.is_current() || self.vdm_alias.is_some() {
            return Err(EBUSY);
        }
        if self.capture != payload::Capture::FragmentWitness || address != 0x18000 ||
            va::GRAPHICS_HIGH_VDM & (va::PAGE - 1) != 0 { return Err(EINVAL); }
        let vm = self.context.vm();
        let page = self.pages.iter_mut().find(|page| page.iova() == address).ok_or(EFAULT)?;
        let alias = page.map_alias_at(vm, va::GRAPHICS_HIGH_VDM,
            mmu::PROT_GPU_SHARED_RW)?;
        self.aliases.push(alias, GFP_KERNEL)?;
        self.vdm_alias = Some((address, va::GRAPHICS_HIGH_VDM));
        self.audit()?;
        Ok(va::GRAPHICS_HIGH_VDM - va::COMPACT_BIAS)
    }

    /// The caller supplies the register builder's decoded VDM stream address.
    /// A standalone termination word affects no shaders or other capture data.
    pub(crate) fn terminate_vdm(&mut self, address: u64) -> Result {
        if self.context.is_current() || address & 3 != 0 { return Err(EBUSY); }
        let address = if (va::COMPACT_BIAS..va::COMPACT_BIAS + 0x78000).contains(&address) {
            address - va::COMPACT_BIAS
        } else { address };
        let base = address & !(va::PAGE - 1);
        let page = self.pages.iter_mut().find(|page| page.iova() == base).ok_or(EFAULT)?;
        page.write_u32(address, 6 << 29)?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Output digest is observation only, never a completion predicate.
    pub(crate) fn verify_pixels(&mut self, clear: bool) -> Result<([u32;8],u32,u32)> {
        let mut sha = crate::g18::fixture::render_pixels::Pixels::new();
        for index in 0..16 {
            let address = payload::OUTPUT + index * va::PAGE;
            let page = self.pages.iter_mut().find(|p|p.iova()==address).ok_or(EFAULT)?;
            page.with_bytes_mut(|bytes| {
                if !sha.page(bytes) {return Err(EINVAL);} Ok(())
            })?;
        }
        let poison=sha.poison; let background=sha.background;
        let digest=sha.finish().ok_or(EINVAL)?;
        if poison!=0 || (clear && background!=65536)
            || (!clear && (background!=47104 || digest!=crate::g18::fixture::render_pixels::COLOR)) {return Err(EIO);}
        Ok((digest,background,poison))
    }

    /// Output digest is observation only, never a completion predicate.
    pub(crate) fn output(&mut self) -> Result<(u64, u32)> {
        let mut hash = 0xcbf2_9ce4_8422_2325u64;
        let mut poison = 0;
        let mut count = 0;
        for page in self.pages.iter_mut().filter(|p|
            (payload::OUTPUT..payload::OUTPUT + payload::OUTPUT_BYTES).contains(&p.iova())) {
            page.with_bytes_mut(|bytes| {
                for word in bytes.chunks_exact(4) {
                    // SAFETY: aligned four-byte word inside retained WC RAM.
                    let value = unsafe { core::ptr::read_volatile(word.as_ptr().cast::<u32>()) };
                    if value == 0xa5a5_a5a5 { poison += 1; }
                    for b in value.to_le_bytes() { hash = (hash ^ u64::from(b)).wrapping_mul(0x100_0000_01b3); }
                    count += 1;
                }
                Ok(())
            })?;
        }
        if count != 65536 { return Err(EFAULT); }
        fence(Ordering::Acquire);
        Ok((hash, poison))
    }
}
