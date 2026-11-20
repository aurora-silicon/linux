// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8103 (G13G) with GPU firmware 27.2: the bring-up handshake, derived from the T8112 one.

use super::*;

impl Uat {
    /// Boot only as far as the legacy PPL handshake. No host tables or queues are published.
    #[cfg(CONFIG_DRM_ASAHI_M3_T8122)]
    pub(crate) fn observe_t8103_peer<'a>(pdev: &'a kernel::platform::Device<kernel::device::Core<'_>>) -> Result<Option<crate::j313::Live<'a>>> {
        use kernel::io::{Io, IoBase};
        let phase = module_parameters::t8112_handshake.value();
        let followup = module_parameters::t8112_active_followup.value();
        if followup > 2 || (followup != 0 && (phase != 14
            // T8103 builds its own power images, so the T8112 power filter is not required.
            || !module_parameters::t8112_idle_policy.value()
            || module_parameters::t8112_post_power_job.value() || module_parameters::t8112_ram_slots.value() != 0)) {
            return Err(EINVAL);
        }
        if module_parameters::t8112_command_engine.value() > 2 { return Err(EINVAL); }
        let classes = module_parameters::t8112_engine_classes.value();
        if classes > 2 || (classes != 0 && (phase != 14 || !module_parameters::t8112_idle_policy.value())) {
            return Err(EINVAL);
        }
        let region = Self::map_region(pdev.as_ref(), c"handoff", HANDOFF_SIZE, true)?;
        // SAFETY: map_region checked the size of the dedicated UAT reservation.
        // Only the aligned shared atomic fields in its protocol header are used.
        let h = unsafe { &*region.map.ptr().cast::<Handoff>() };
        let req = pdev.io_request_by_name(c"asc").ok_or(EINVAL)?;
        let mapping = req.iomap_sized::<{ crate::regs::ASC_CTL_SIZE }>()?;
        let asc = mapping.as_view();
        let ctl = asc.read32(0x44);
        let mut ap = h.magic_ap.load(Ordering::Acquire);
        let mut fw = h.magic_fw.load(Ordering::Acquire);
        dev_info!(pdev.as_ref(), "G13G pre-init: ASC control={:#x} handoff AP={:#x} FW={:#x}\n", ctl, ap, fw);
        // A previous load of this driver that stopped the firmware leaves both magic words
        // behind. With the ASC stopped and nothing else in the handoff, start over.
        const STALE_MAGIC: u64 = 0x4b1d000000000003;
        if ctl & 0x10 == 0 && fw == STALE_MAGIC && (ap == 0 || ap == STALE_MAGIC) {
            // The ASC is stopped, so neither side holds the locks any more.
            dev_info!(pdev.as_ref(), "G13G clearing the handoff a stopped earlier load left: lock AP={} FW={} turn={}\n",
                h.lock_ap.load(Ordering::Acquire), h.lock_fw.load(Ordering::Acquire), h.turn.load(Ordering::Acquire));
            h.lock_ap.store(0, Ordering::Release);
            h.lock_fw.store(0, Ordering::Release);
            h.turn.store(0, Ordering::Release);
            h.magic_ap.store(0, Ordering::Release);
            h.magic_fw.store(0, Ordering::Release);
            ap = 0;
            fw = 0;
        }
        if ctl & 0x10 != 0 || ap != 0 || fw != 0 ||
            h.lock_ap.load(Ordering::Acquire) != 0 || h.lock_fw.load(Ordering::Acquire) != 0 {
            return Err(EBUSY);
        }
        // The 26B123 firmware advertised this word to the v2 host.
        const G14G_26B123_MAGIC: u64 = 0x4b1d000000000003;
        let device: &device::Device = pdev.as_ref();
        let peer = kernel::soc::apple::rtkit::RtKit::<T8112Peer>::new(
            pdev.as_ref(), None, 0, Arc::new(ARef::from(device), GFP_KERNEL)?,
        )?;
        let ttbs = Self::map_region(device, c"ttbs", UAT_PGSZ, true)?;
        let tables = Self::map_region(device, c"pagetables", UAT_PGSZ, true)?;
        let sgx = crate::regs::Resources::new(pdev)?;
        let shared_l2 = Self::map_region(device, c"shared-l2", UAT_PGSZ, true)?;
        let l2_state = || {
            // SAFETY: shared-l2 is one loader-reserved 16 KiB table.
            let l2 = unsafe { &*shared_l2.map.ptr().cast::<[AtomicU64; UAT_PGSZ / 8]>() };
            let mut first = None;
            let mut used = 0;
            for (index, entry) in l2.iter().enumerate() {
                let value = entry.load(Ordering::Acquire);
                if value != 0 {
                    first.get_or_insert((index, value));
                    used += 1;
                }
            }
            (used, first)
        };
        let snapshot = |region: &UatRegion| {
            // SAFETY: Both reservations cover a full aligned page. Sample the shared
            // table words without interpreting them as CPU pointers.
            let words = unsafe { &*region.map.ptr().cast::<[AtomicU64; 4]>() };
            words.each_ref().map(|v| v.load(Ordering::Acquire))
        };
        dev_info!(device, "G13G stopped TTBAT={:x?} root={:x?}\n", snapshot(&ttbs), snapshot(&tables));
        dev_info!(device, "G13G stopped shared-L2 PA={:#x} entries={:x?}\n", shared_l2.base, l2_state());
        // Qualify the G15 host window without giving the firmware an InitData pointer.
        // Install while the ASC is stopped. The isolated leaf maps only a zeroed,
        // read-only host-owned page and is retained until the ASC stops again.
        let cfg = &crate::hw::t8103::HWCONFIG;
        let geometry = UatGeometry { ias: cfg.uat_ias, oas: cfg.uat_oas };
        let va = geometry.kernel_range().start;
        let page = kernel::page::Page::alloc_page(GFP_KERNEL | __GFP_ZERO)?;
        let mut shadow = UatPageTable::new(geometry.ias, geometry.oas)?;
        shadow.enable_clean_poc()?;
        let range = va..va + UAT_PGSZ as u64;
        shadow.alloc_pages(range.clone())?;
        shadow.map_pages(range, page.phys(), PROT_FW_SHARED_RO, false)?;
        if shadow.translate_iova(va)? != page.phys() || l2_state().0 != 0 {
            return Err(EINVAL);
        }
        let mask = ((1u64 << geometry.oas) - 1) & !(UAT_PGMSK as u64);
        // SAFETY: This exclusively owned shadow and all its page-table pages outlive this
        // walk. Each intermediate address is extracted from our own table.
        let leaf_table = unsafe {
            let upper = pgtable::with_root_entries(None, shadow.ttb(), 3,
                |words| words[2].load(Ordering::Acquire))?;
            if upper & 3 != 3 { return Err(EINVAL); }
            pgtable::with_root_entries(None, upper & mask, 1,
                |words| words[0].load(Ordering::Acquire))?
        };
        if leaf_table & 3 != 3 { return Err(EINVAL); }
        // SAFETY: This aligned word is in the ADT shared-L2 reservation.
        let slot = unsafe { &*shared_l2.map.ptr().cast::<AtomicU64>() };
        let want_graph = phase >= 2;
        // The extended G15G and primary T6050 profile words are controls.
        let version = match phase {
            3 | 5 => Some(0x0c89_c357_8392_04b8),
            4 | 6 => Some(0x0c8e_4339_8728_04c0),
            // Published T8112 host submission, Stela #170, build 26B123.
            7 | 9..=14 => Some(0x0c8a_8342_8392_04c8),
            _ => None,
        };
        let mut modern = None;
        let mut init_va = 0;
        let mut graph: Option<Arc<crate::gpu::GpuManagerG13V13_5>> = None;
        let mut binding: Option<crate::binding::Owner> = None;
        let mut drm_keep = None;
        let mut pool: Option<crate::gpu::J313Pool> = None;
        let mut entries = KVec::from_elem(0u64, UAT_PGSZ / 8, GFP_KERNEL)?;
        entries[0] = leaf_table;
        let mut lower_root = 0;
        if want_graph {
            #[cfg(not(CONFIG_DRM_ASAHI_G15))]
            return Err(EOPNOTSUPP);
            #[cfg(CONFIG_DRM_ASAHI_G15)]
            {
                use kernel::dma::{Device as _, DmaMask};
                // SAFETY: The mask is the real T8112 UAT's physical width.
                unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(geometry.oas)?)? };
                let drm = kernel::drm::UnregisteredDevice::<driver::AsahiDrmDriver>::new(pdev, Ok(()))?;
                // SAFETY: The unregistered DRM instance is private to this bound probe.
                // The local owner closes on all returns.
                binding = Some(unsafe { crate::binding::Owner::new(pdev.as_ref(), &drm)? });
                let regions = AdoptedRegions {
                    handoff: region.base..region.base + UAT_PGSZ as u64,
                    ttbs: ttbs.base..ttbs.base + UAT_PGSZ as u64,
                    kernel_root: tables.base,
                };
                let uat = Self::new_adopted(&drm, geometry, &regions)?;
                let control = crate::gpu::GpuManagerG13V13_5::build_graph(
                    &drm, &sgx, cfg, KBox::new(uat, GFP_KERNEL)?, false, version,
                )?;
                init_va = if phase >= 5 {
                    let header = match phase {
                        11..=14 => {
                            let node = device.fwnode().ok_or(ENODEV)?;
                            let sgx = pdev.resource_by_name(c"sgx").ok_or(EINVAL)?;
                            let copy_bytes: u32 = node.property_read(c"apple,coredump-copy-buffer").required_by(device)?;
                            let interrupt: u64 = node.property_read(c"apple,meta-sw-interrupt").required_by(device)?;
                            if sgx.start() != 0x204000000 || sgx.size() < 0xe14000 ||
                                copy_bytes != 0x8000 || interrupt & !0x3fff != 0x23b104000 {
                                return Err(EINVAL);
                            }
                            Some(crate::gpu::G13DirectoryHeader::J313 { sgx: sgx.start(), sgx_size: sgx.size() as u64, interrupt, copy_bytes, live: phase >= 12, first_job: phase >= 13 })
                        }
                        9 => Some(crate::gpu::G13DirectoryHeader::Zero),
                        10 => Some(crate::gpu::G13DirectoryHeader::PrivatePage),
                        _ => None,
                    };
                    let candidate = crate::gpu::G13TrialRoot::build(
                        &control, version.ok_or(EINVAL)?, header,
                    )?;
                    if module_parameters::t8112_drm.value() {
                        // Runtime command slots, before the window's tables are mirrored.
                        let mut alloc = crate::gpu::GpuManager::alloc(&*control);
                        pool = Some(crate::gpu::J313Pool::new(&mut alloc)?);
                    }
                    candidate.with(|r, _| {
                        let word = r.ver_info.iter().enumerate()
                            .fold(0u64, |v, (i, &w)| v | (u64::from(w) << (16*i)));
                        dev_info!(device, "G13G encoded root word={:#018x} runtime={:?} status={:?}\n",
                            word, r.runtime_pointers, r.status_record);
                        pgtable::clean_poc(core::ptr::from_ref(r).cast(), core::mem::size_of_val(r));
                    });
                    if module_parameters::t8112_g13_dump.value() == 1 {
                        candidate.dump_images(device);
                    }
                    let address = candidate.gpu_va().get();
                    modern = Some(candidate);
                    address
                } else {
                    control.trial_root()
                };
                let root = control.trial_uat().kernel_window_root();
                // SAFETY: The private control graph owns both table levels.
                unsafe {
                    let upper = pgtable::with_root_entries(None, root, 3,
                        |w| w[2].load(Ordering::Acquire))?;
                    if upper & 3 != 3 { return Err(EINVAL); }
                    pgtable::with_root_entries(None, upper & mask, entries.len(), |w| {
                        for (entry, word) in entries.iter_mut().zip(w) {
                            *entry = word.load(Ordering::Acquire);
                        }
                    })?;
                }
                // The window's final 4 GiB is reserved for MMIO. The trial graph
                // has one MMIO subtree only for the stopped 26B123 board construction.
                let first_io = (0xf_0000_0000u64 >> 25) as usize;
                let mmio_tables = entries[first_io..].iter().filter(|w| **w != 0).count();
                if mmio_tables != usize::from(matches!(phase, 11..=14)) { return Err(EINVAL); }
                for &w in entries.iter().filter(|w| **w != 0) {
                    if w & 3 != 3 { return Err(EINVAL); }
                    crate::g15::uat::clean_page(w & mask)?;
                }
                crate::g15::uat::clean_lower(control.trial_uat())?;
                lower_root = control.trial_uat().kernel_lower_root();
                dev_info!(device, "G13G InitData phase={} word={:#018x} root={:#x} RAM tables={} MMIO tables={}\n",
                    phase, version.unwrap_or(0x00b0_0601_1f28_6ba0), init_va, entries[..first_io].iter().filter(|w| **w != 0).count(), mmio_tables);
                graph = Some(control);
                drm_keep = Some(drm);
            }
        }
        // Construction-only profiles must return before touching the live UAT.
        if matches!(phase, 9..=11) {
            dev_info!(device, "G13G directory construction verified with ASC stopped\n");
            drop(modern);
            drop(graph);
            drop(binding);
            return Ok(None);
        }
        // SAFETY: These are aligned words of two dedicated one-page reservations.
        let shared = unsafe { &*shared_l2.map.ptr().cast::<[AtomicU64; UAT_PGSZ / 8]>() };
        let context = unsafe { &*ttbs.map.ptr().cast::<SlotTTBS>() };
        if l2_state().0 != 0 || snapshot(&ttbs) != [0; 4] { return Err(EBUSY); }
        let gate_mask = crate::regs::G17_GATING_DISABLE;
        let previous_gating = match module_parameters::t8112_fender_gating.value() {
            0 => None,
            mode @ (1 | 2) if phase == 14 && module_parameters::t8112_idle_policy.value()
                && module_parameters::t8112_ram_slots.value() == 0 => {
                let (clear, set) = if mode == 1 { (0, crate::regs::G17_GATING_HOLD) } else { (gate_mask, 0) };
                Some(sgx.t8112_fender_gating(clear, set)? & gate_mask)
            }
            _ => return Err(EINVAL),
        };
        let open_guard = if module_parameters::t8112_sgx_open.value() {
            if phase != 14 || !module_parameters::t8112_idle_policy.value()
                || !module_parameters::t8112_event_completion.value() { return Err(EINVAL); }
            let (before, after) = sgx.t8112_sgx_open(true);
            let regs = &sgx;
            let guard = kernel::types::ScopeGuard::new(move || {
                regs.t8112_sgx_open(before & 1 != 0);
            });
            if after & 1 == 0 { return Err(EIO); }
            Some(guard)
        } else { None };
        let irq_bit = module_parameters::t8112_gpc_irq_bit.value();
        let irq_guard = if irq_bit == 0 {
            None
        } else {
            if !matches!(irq_bit, 1 | 2) || phase != 14 || !module_parameters::t8112_idle_policy.value()
                || !module_parameters::t8112_event_completion.value() { return Err(EINVAL); }
            let was_set = sgx.t8112_gpc_irq_enable() & irq_bit != 0;
            let regs = &sgx;
            let guard = kernel::types::ScopeGuard::new(move || {
                regs.t8112_update_gpc_irq(irq_bit, was_set);
            });
            if sgx.t8112_update_gpc_irq(irq_bit, true) & irq_bit == 0 { return Err(EIO); }
            Some(guard)
        };
        let pmp_vote = module_parameters::t8112_pmp_vote.value();
        if pmp_vote {
            if phase != 14 || !module_parameters::t8112_idle_policy.value() ||
                module_parameters::t8112_ram_slots.value() != 0 { return Err(EINVAL); }
            #[cfg(not(CONFIG_DRM_ASAHI_M3_T8122))]
            return Err(ENOTSUPP);
            #[cfg(CONFIG_DRM_ASAHI_M3_T8122)]
            {
                // SAFETY: the live GPU consumer links to its declared PMP supplier. The
                // bridge waits for the report and serializes the untagged endpoint reply.
                kernel::error::to_result(unsafe { kernel::bindings::apple_pmp_link_device(device.as_raw()) })?;
                let result = unsafe { kernel::bindings::apple_pmp_set_device_power(0x0f, 5, 1) };
                dev_info!(device, "G13G PMP AGX device 5 acquire result={}\n", result);
                kernel::error::to_result(result)?;
            }
        }
        for (word, value) in shared.iter().zip(entries.iter()) {
            if *value != 0 { word.store(*value, Ordering::Release); }
        }
        if want_graph {
            context.ttb0.store(lower_root | TTBR_VALID, Ordering::Release);
            context.ttb1.store(tables.base | TTBR_VALID, Ordering::Release);
            pgtable::clean_poc(ttbs.map.ptr(), 16);
            let job = module_parameters::t8112_job_context.value() as usize;
            if job != 0 {
                if job >= UAT_NUM_CTX { return Err(EINVAL); }
                // A client context for the kernel-owned job: the same two trees as context 0.
                // SAFETY: the slot lies inside the mapped, size-checked TTBAT reservation.
                let client = unsafe { &*ttbs.map.ptr().cast::<SlotTTBS>().add(job) };
                // As Vm::bind writes it for other firmware: both entries
                // carry the context identifier in bits 63:48.
                let id = if module_parameters::t8112_context_id_field.value() {
                    (job as u64) << TTBR_ASID_SHIFT
                } else { 0 };
                client.ttb0.store(lower_root | TTBR_VALID | id, Ordering::Release);
                client.ttb1.store(tables.base | TTBR_VALID | id, Ordering::Release);
                pgtable::clean_poc(ttbs.map.ptr(), (job + 1) * 16);
            }
        }
        pgtable::clean_poc(shared_l2.map.ptr(), UAT_PGSZ);
        pgtable::poc_barrier();
        dev_info!(device, "G13G preboot host window VA={:#x} first entry={:#x}\n", va, entries[0]);
        let mut peer_box = KBox::pin(peer, GFP_KERNEL)?;
        let mut peer = peer_box.as_mut();
        h.magic_ap.store(G14G_26B123_MAGIC, Ordering::Release);
        fence(Ordering::SeqCst);
        asc.write32(ctl | 0x10, 0x44);
        let run = asc.read32(0x44);
        let start = Instant::<Monotonic>::now();
        while start.elapsed() < Delta::from_millis(1000) && h.magic_fw.load(Ordering::Acquire) != G14G_26B123_MAGIC {
            fsleep(Delta::from_millis(1));
        }
        let observed = h.magic_fw.load(Ordering::Acquire);
        if observed == G14G_26B123_MAGIC {
            let result = peer.as_mut().boot();
            dev_info!(pdev.as_ref(), "G13G RTKit boot={:?} firmware_ep={} doorbell_ep={}\n", result, peer.as_mut().has_endpoint(0x20), peer.as_mut().has_endpoint(0x21));
            if result.is_ok() {
                dev_info!(device, "G13G running shared-L2 PA={:#x} entries={:x?}\n", shared_l2.base, l2_state());
                match sgx.get_gpu_id() {
                    Ok(id) => dev_info!(device, "G13G register identity {:?}{:?} {:?}, clusters={} active_cores={} mask={:x?}\n",
                                        id.gpu_gen, id.gpu_variant, id.gpu_rev, id.num_clusters,
                                        id.total_active_cores, id.core_masks),
                    Err(e) => dev_warn!(device, "G13G register identity error {:?}\n", e),
                }
                let cmd = peer.as_mut().start_endpoint(0x20);
                let doorbell = peer.as_mut().start_endpoint(0x21);
                if graph.is_some() {
                    if cmd.is_ok() && doorbell.is_ok() && snapshot(&tables)[2] & mask == shared_l2.base
                        && context.ttb0.load(Ordering::Acquire) == lower_root | TTBR_VALID
                        && context.ttb1.load(Ordering::Acquire) == tables.base | TTBR_VALID {
                        let power_sample = |stage: &str| {
                            if phase == 14 {
                                if let Some(root) = modern.as_ref() {
                                    if let Err(e) = root.report_power_progress(stage, sgx.t8112_operating_point()) {
                                        dev_warn!(device, "G13G {} power sample failed {:?}\n", stage, e);
                                    }
                                }
                            }
                        };
                        let kick_fence = |peer: &mut core::pin::Pin<&mut kernel::soc::apple::rtkit::RtKit<T8112Peer>>| -> Result {
                            let root = modern.as_ref().ok_or(EINVAL)?;
                            let start = Instant::<Monotonic>::now();
                            while module_parameters::t8112_command_service.value()
                                && root.command_service()? != (2, 0)
                                && start.elapsed() < Delta::from_millis(500) {
                                fsleep(Delta::from_millis(1));
                            }
                            dev_info!(device, "G13G command service (stage, request)={:?}\n", root.command_service());
                            root.publish_startup_fence(sgx.t8112_operating_point())?;
                            let kick = peer.as_mut().send_message(0x21, (0x83u64 << 48) | (4 * crate::gpu::G13TrialRoot::signal_group() + crate::gpu::G13TrialRoot::signal_engine()) as u64);
                            dev_info!(device, "G13G first compute fence kick={:?}\n", kick);
                            kick
                        };
                        let mut power_cycled = false;
                        power_sample("before-init");
                        let sent = peer.as_mut().send_message(0x20, crate::gpu::init_message(init_va));
                        dev_info!(device, "G13G InitData-only post phase={} result={:?}\n", phase, sent);
                        // Keep the statistics consumer moving through every wait.
                        let nap = |ms: i64| {
                            let start = Instant::<Monotonic>::now();
                            while start.elapsed() < Delta::from_millis(ms) {
                                if let Some(root) = modern.as_ref() { let _ = root.drain_statistics(); }
                                fsleep(Delta::from_millis(1));
                            }
                        };
                        nap(1000);
                        if matches!(phase, 13 | 14) && sent.is_ok() {
                            dev_info!(device, "G13G before publication: GPC interrupt enable={:#x}\n",
                                      sgx.t8112_gpc_irq_enable());
                        }
                        if phase == 14 && sent.is_ok() {
                            power_sample("after-init");
                            let policy = module_parameters::t8112_idle_policy.value();
                            let scratch = module_parameters::t8112_ram_slots.value() != 0 ||
                                module_parameters::t8112_dashboard_ram.value();
                            let mut verified = false;
                            let audit = scratch || module_parameters::t8112_performance_control.value()
                                || module_parameters::t8112_cre.value();
                            if !policy || audit {
                                if let Some(root) = modern.as_ref() {
                                    let walk = |va: u64| -> Result<u64> {
                                        let va = va & ((1u64 << 39) - 1);
                                        let i = ((va >> 25) & 2047) as usize;
                                        if va >> 36 != 2 || context.ttb1.load(Ordering::Acquire) != tables.base | TTBR_VALID ||
                                            snapshot(&tables)[2] & (mask | 3) != shared_l2.base | 3 { return Err(EFAULT); }
                                        let l3 = shared[i].load(Ordering::Acquire);
                                        // Only follow the L3 page retained by this graph at this index.
                                        if l3 & 3 != 3 || l3 != entries[i] { return Err(EFAULT); }
                                        let j = ((va >> 14) & 2047) as usize;
                                        // SAFETY: this entry names the live, driver-owned RAM table checked above.
                                        unsafe { pgtable::with_root_entries(None, l3 & mask, j+1,
                                            |words| words[j].load(Ordering::Acquire)) }
                                    };
                                    let result = root.audit_power_inputs(walk, mask);
                                    verified = result.is_ok();
                                    dev_info!(device, "G13G audit upper={:#x} shared_l2={:#x} result={:?}\n",
                                              context.ttb1.load(Ordering::Acquire), shared_l2.base, result);
                                }
                            }
                            // Same owners and observation interval as the first fence,
                            // with both producer indices still zero and no queue kick.
                            if policy && (!audit || verified) {
                                if let Some(root) = modern.as_ref() {
                                    let set_policy = |peer: &mut core::pin::Pin<&mut kernel::soc::apple::rtkit::RtKit<T8112Peer>>, allow_off| -> Result {
                                        let (target, generation) = root.publish_idle_policy(allow_off)?;
                                        peer.as_mut().send_message(0x21, 0x0084_0000_0000_0011)?;
                                        let start = Instant::<Monotonic>::now();
                                        let mut last = None;
                                        loop {
                                            let state = (root.idle_progress()?, sgx.t8112_operating_point());
                                            if last != Some(state) {
                                                power_sample(if allow_off { "allow-off" } else { "keep-powered" });
                                                last = Some(state);
                                            }
                                            if state.0 == [target; 3] && root.idle_acknowledgement()? == generation {
                                                return Ok(());
                                            }
                                            // G13G's first scheduler pass follows its first DVFS
                                            // transition and power-controller start.
                                            if start.elapsed() >= Delta::from_millis(i64::from(module_parameters::t8112_g13_idle_ms.value().max(500))) {
                                                return Err(ETIMEDOUT);
                                            }
                                            fsleep(Delta::from_millis(1));
                                        }
                                    };
                                    // J613 extended order: the opening record retires with
                                    // InitData, then each engine class retires on its own kick.
                                    let base = crate::gpu::G13TrialRoot::control_base();
                                    let mut opened = base == 0 || root.idle_progress() == Ok([1; 3]);
                                    if base != 0 {
                                        dev_info!(device, "G13G opening control retired={} progress={:?}\n",
                                                  opened, root.idle_progress());
                                    }
                                    for ordinal in 0..base.saturating_sub(1) {
                                        if !opened { break; }
                                        let class = (|| -> Result {
                                            let target = root.publish_engine_class(ordinal)?;
                                            peer.as_mut().send_message(0x21, 0x0084_0000_0000_0011)?;
                                            let start = Instant::<Monotonic>::now();
                                            while root.idle_progress()? != [target; 3] {
                                                if start.elapsed() >= Delta::from_millis(500) { return Err(ETIMEDOUT); }
                                                fsleep(Delta::from_millis(1));
                                            }
                                            Ok(())
                                        })();
                                        dev_info!(device, "G13G engine class pipe={} engine={} retired={:?} progress={:?}\n",
                                                  ordinal / 2, ordinal % 2 + 1, class, root.idle_progress());
                                        opened = class.is_ok();
                                    }
                                    for allow_off in [false, true] {
                                        if !opened { break; }
                                        let policy = set_policy(&mut peer, allow_off);
                                        dev_info!(device, "G13G idle policy allow_off={} retired={:?}\n", allow_off, policy);
                                        if policy.is_err() { break; }
                                        power_cycled = allow_off;
                                        if !allow_off && followup == 2 {
                                            let held_job = (|| -> Result {
                                                kick_fence(&mut peer)?;
                                                let start = Instant::<Monotonic>::now();
                                                let mut sampled = 0;
                                                let mut rekick = module_parameters::t8112_rekick_ms.value();
                                                while !root.startup_fence_complete()? {
                                                    if rekick != 0 && start.elapsed() >= Delta::from_millis(rekick as i64) {
                                                        rekick = 0;
                                                        let again = peer.as_mut().send_message(0x21, (0x83u64 << 48)
                                                            | (4 * crate::gpu::G13TrialRoot::signal_group() + crate::gpu::G13TrialRoot::signal_engine()) as u64);
                                                        dev_info!(device, "G13G repeat kick={:?}\n", again);
                                                    }
                                                    if module_parameters::t8112_compute_job.value()
                                                        && start.elapsed() >= Delta::from_millis(1000 + 200 * sampled)
                                                        && sampled < 2 {
                                                        sampled += 1;
                                                        root.report_job_gates("parked")?;
                                                        if sampled == 1 && module_parameters::t8112_private_scan.value() & 3 != 0 {
                                                            let found = t8112_private_scan(device, &root.job_anchors(), module_parameters::t8112_private_scan.value());
                                                            dev_info!(device, "G13G private scan={:?}\n", found);
                                                        }
                                                    }
                                                    if start.elapsed() >= Delta::from_millis(module_parameters::t8112_job_wait_ms.value() as i64) { return Err(ETIMEDOUT); }
                                                    fsleep(Delta::from_millis(1));
                                                }
                                                Ok(())
                                            })();
                                            dev_info!(device, "G13G held fence completion={:?}\n", held_job);
                                            if module_parameters::t8112_drm.value() && held_job.is_ok()
                                                && modern.is_some() && graph.is_some() && pool.is_some()
                                                && drm_keep.is_some() && binding.is_some() {
                                                // Hand the running firmware to the DRM backend instead of stopping it.
                                                let asc_phys = pdev.resource_by_name(c"asc").ok_or(EINVAL)?.start();
                                                let asc_ctl = crate::j313::AscControl::new(asc_phys)?;
                                                let (Some(root), Some(control), Some(slots), Some(drm), Some(owner)) =
                                                    (modern.take(), graph.take(), pool.take(), drm_keep.take(), binding.take())
                                                    else { return Err(EINVAL); };
                                                let window = crate::j313::Window::new(
                                                    control.trial_uat().kernel_window_root(),
                                                    shared_l2.map.ptr().cast(), mask)?;
                                                drop(irq_guard);
                                                drop(open_guard);
                                                // The firmware keeps using these until it is stopped; their
                                                // mappings stay for the life of the boot.
                                                core::mem::forget(region);
                                                core::mem::forget(ttbs);
                                                core::mem::forget(tables);
                                                core::mem::forget(shared_l2);
                                                core::mem::forget(page);
                                                core::mem::forget(shadow);
                                                core::mem::forget(mapping);
                                                dev_info!(device, "G13G handing the running firmware to the DRM backend\n");
                                                return Ok(Some(crate::j313::Live {
                                                    peer: peer_box, root, graph: control, pool: slots, asc: asc_ctl,
                                                    drm, binding: owner, resources: sgx, window,
                                                }));
                                            }
                                            if module_parameters::t8112_private_scan.value() & 4 != 0 {
                                                let found = t8112_private_scan(device, &root.job_anchors(), 3);
                                                dev_info!(device, "G13G private scan after wait={:?}\n", found);
                                            }
                                            if ((held_job.is_ok() && module_parameters::t8112_job_order.value() == 1)
                                                    || module_parameters::t8112_job_order.value() == 2)
                                                && module_parameters::t8112_compute_job.value() {
                                                let second = (|| -> Result {
                                                    root.publish_second()?;
                                                    peer.as_mut().send_message(0x21, (0x83u64 << 48)
                                                        | (4 * crate::gpu::G13TrialRoot::signal_group() + crate::gpu::G13TrialRoot::signal_engine()) as u64)?;
                                                    let start = Instant::<Monotonic>::now();
                                                    let mut sampled = 0;
                                                    while !root.second_complete()? {
                                                        if start.elapsed() >= Delta::from_millis(500 + 200 * sampled) && sampled < 2 {
                                                            sampled += 1;
                                                            root.report_job_gates("second")?;
                                                        }
                                                        if start.elapsed() >= Delta::from_millis(i64::from(module_parameters::t8112_g13_idle_ms.value().max(500))) { return Err(ETIMEDOUT); }
                                                        fsleep(Delta::from_millis(1));
                                                    }
                                                    Ok(())
                                                })();
                                                dev_info!(device, "G13G second command (compute) completion={:?}\n", second);
                                                if second.is_err() { break; }
                                            }
                                            if held_job.is_err() { break; }
                                        }
                                        if allow_off || followup == 0 {
                                            fsleep(Delta::from_millis(100));
                                        }
                                        power_sample(if allow_off { "released" } else { "held" });
                                    }
                                }
                            }
                            nap(1000);
                        }
                        if module_parameters::t8112_compute_job.value() && sgx.t8112_operating_point() & 0xf != 0 {
                            let latch = sgx.t8112_fault_latch();
                            dev_info!(device, "G13G core fault latch={:#x} valid={} reason={} client={} address={:#x}\n",
                                      latch, latch & 1, (latch >> 1) & 7, (latch >> 9) & 0xff, (latch >> 24) & !0x3f);
                        }
                        // After the allow-off control retires, the firmware
                        // publishes power-off by clearing shared status +0x10.
                        if power_cycled {
                            if let Some(root) = modern.as_ref() {
                                let start = Instant::<Monotonic>::now();
                                while root.firmware_running() != Ok(0) && start.elapsed() < Delta::from_millis(500) {
                                    let _ = root.drain_statistics();
                                    fsleep(Delta::from_millis(1));
                                }
                                dev_info!(device, "G13G stop acknowledgement status+0x10={:?} after {} ms\n",
                                          root.firmware_running(), start.elapsed().as_millis());
                            }
                        }
                        // With work still parked the allow-off control cannot
                        // complete. Ask RTKit for acknowledged power-off of both sides while
                        // every mapping is still in place, before the ASC run bit is cleared.
                        let forced = module_parameters::t8112_forced_stop.value();
                        if forced != 0 {
                            if let Some(root) = modern.as_ref() {
                                let before = root.firmware_running();
                                if forced == 2 || before != Ok(0) {
                                    let start = Instant::<Monotonic>::now();
                                    // SAFETY: the RTKit instance is live and owned by this probe.
                                    let off = kernel::error::to_result(unsafe {
                                        kernel::bindings::apple_rtkit_poweroff(peer.as_raw())
                                    });
                                    dev_info!(device, "G13G forced stop status+0x10 before={:?} power-off={:?} after {} ms status+0x10={:?} ASC={:#x}\n",
                                              before, off, start.elapsed().as_millis(), root.firmware_running(), asc.read32(0x44));
                                }
                            }
                        }
                        power_sample("before-stop");
                        let post_power_job = power_cycled && module_parameters::t8112_post_power_job.value()
                            && module_parameters::t8112_ram_slots.value() == 0;
                        if (phase == 13 || post_power_job) && sent.is_ok() {
                            match kick_fence(&mut peer) {
                                Ok(()) => fsleep(Delta::from_millis(1000)),
                                Err(e) => dev_info!(device, "G13G compute fence submission failed {:?}\n", e),
                            }
                        }
                    }
                }
                fsleep(Delta::from_millis(50));
                dev_info!(device, "G13G start endpoints command={:?} doorbell={:?} TTBAT={:x?} root={:x?} lock_fw={} turn={} slot={}\n",
                          cmd, doorbell, snapshot(&ttbs), snapshot(&tables),
                          h.lock_fw.load(Ordering::Acquire), h.turn.load(Ordering::Acquire), h.cur_slot.load(Ordering::Acquire));
            }
        }
        asc.write32(asc.read32(0x44) & !0x10, 0x44);
        let stopped = asc.read32(0x44);
        dev_info!(pdev.as_ref(), "G13G pre-init: ASC started={:#x} FW={:#x} stopped={:#x}\n", run, observed, stopped);
        if stopped & 0x10 != 0 {
            shadow.retain();
            core::mem::forget(page);
            core::mem::forget(modern);
            core::mem::forget(graph);
            return Err(EBUSY);
        }
        if phase >= 12 {
            if let Some(root) = modern.as_ref() {
                if phase == 14 {
                    if let Err(e) = root.report_stopped_power() {
                        dev_warn!(device, "G13G stopped power sample failed {:?}\n", e);
                    }
                }
                dev_info!(device, "G13G stopped command service (stage, request)={:?}\n", root.command_service());
                root.report_stopped_fence();
                root.report_stopped_receive();
                        if module_parameters::t8112_g13_dump.value() == 2 {
                            root.dump_images(device);
                        }
                let (cold, started, ready) = root.stopped_status();
                dev_info!(device, "G13G final shared status cold={} started={} ready={}\n",
                          cold, started, ready);
            }
        }
        pgtable::clean_invalidate_poc(shared_l2.map.ptr(), UAT_PGSZ);
        pgtable::poc_barrier();
        let final_entry = slot.load(Ordering::Acquire);
        dev_info!(device, "G13G stopped host window slot={:#x} expected={:#x}\n", final_entry, entries[0]);
        if shared.iter().zip(entries.iter()).any(|(w,e)| {
            let value = w.load(Ordering::Acquire);
            value != 0 && value != *e
        }) {
            shadow.retain();
            core::mem::forget(page);
            core::mem::forget(modern);
            core::mem::forget(graph);
            return Err(EBUSY);
        }
        if want_graph {
            context.ttb0.store(0, Ordering::Release);
            context.ttb1.store(0, Ordering::Release);
            pgtable::clean_poc(ttbs.map.ptr(), 16);
        }
        for (word, value) in shared.iter().zip(entries.iter()) {
            if *value != 0 { word.store(0, Ordering::Release); }
        }
        pgtable::clean_poc(shared_l2.map.ptr(), UAT_PGSZ);
        pgtable::poc_barrier();
        mem::tlbi_all();
        mem::sync();
        h.magic_ap.store(ap, Ordering::Release);
        #[cfg(CONFIG_DRM_ASAHI_M3_T8122)]
        if pmp_vote {
            // SAFETY: the ASC was stopped and its UAT mappings withdrawn while power was held.
            let result = unsafe { kernel::bindings::apple_pmp_set_device_power(0x0f, 5, 0) };
            dev_info!(device, "G13G PMP AGX device 5 release result={}\n", result);
            kernel::error::to_result(result)?;
        }
        drop(irq_guard);
        drop(open_guard);
        if let Some(previous) = previous_gating {
            sgx.t8112_fender_gating(gate_mask, previous)?;
        }
        drop(modern);
        drop(graph);
        drop(binding);
        if observed != G14G_26B123_MAGIC { return Err(ETIMEDOUT); }
        Ok(None)
    }
}
