// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 firmware graph backed by the shared G17 object and GPUVM owners.

use super::*;
use crate::g17::fw::m5::initdata as m;

const BASE: u64 = 0xffff_fc20_0000_0000;
const PRIVATE: u64 = BASE + 0x2_0000;
const REGION_C: u64 = BASE + 0x1a_0000;
const ROOTS: u64 = BASE + 0x1a_8000;
const REGION_A: u64 = BASE + 0x1b_8000;
const FWCTL: u64 = BASE + 0x1c_0000;
const QOS: u64 = BASE + 0x1c_8000;
const AON: u64 = BASE + 0x1d_0000;
/// The always-on block the policy names, where macOS 27.0 keeps it on the J813: its policy's AON
/// pointer (bundle +0xb858, read under aura) is the kernel window's 0xc07b_8000, right above the
/// shared cluster. The 25G76 address, [`AON`], holds an empty page there.
const COMMON26_AON: u64 = BASE + 0xc07b_8000;
const KICK_SHARED: u64 = BASE + 0x1d_8000;
const BUNDLE: u64 = BASE + 0xc078_8000;
const COMPLETION_HIGH: u64 = BASE + 0xc080_0000;
const COMPLETION_LOW: u64 = 0x70_0184_8000;
/// The completion rings, where macOS 27.0 keeps them on the J813 (its own UAT, read under aura:
/// four 0x20000 backings on this stride, the log buffers right above them).
const COMMON26_COMPLETION_HIGH: u64 = BASE + 0xc07e_8000;
const COMMON26_COMPLETION_LOW: u64 = 0x72_0184_8000;
const COMPLETION_STRIDE: u64 = 0x2_8000;
const PB: u64 = BASE + 0x15e_0000;
const UMA: u64 = BASE + 0x15e_8000;
/// The log buffer each J813 coprocessor is handed on the crashlog endpoint, by [`Role`]: the
/// addresses and the size macOS 27.0 publishes there.
const FW_LOG: [u64; 2] = [BASE + 0xc089_8000, BASE + 0xc088_8000];
const FW_LOG_SIZE: usize = 0x8000;

/// Lower-root entry 1 of the M5 layout (Mesa's driver window), linked before the firmware starts
/// as the bare-metal J775D boot has always run.
const BOOT_LINK: u64 = 0x10_0008_0000;

impl InitData {
    /// The kernel lower-root ranges a bare-metal M5 firmware reaches, whose top-level links must
    /// exist before it starts: entry 1, the descriptor tables and completion rings (entry 7) and
    /// the first page of the canonical arena (`canonical`, entry 8), where queues go later.
    pub(in crate::g17) fn m5_lower_links(canonical: core::ops::Range<u64>, graph: &m::Graph)
        -> [core::ops::Range<u64>; 8]
    {
        let page = |address: u64| address..address + mmu::UAT_PGSZ as u64;
        let low = if graph.is_common26() { COMMON26_COMPLETION_LOW } else { COMPLETION_LOW };
        let ring = |index: u64| page(low + index * COMPLETION_STRIDE);
        let views = graph.profile.region_view_low_addrs();
        [page(BOOT_LINK), page(views[0]), page(views[1]),
            ring(0), ring(1), ring(2), ring(3), page(canonical.start)]
    }

    /// Diagnostic-only read of an aligned word within the existing policy allocation.
    /// Callers hold the device state mutex, retaining this mapping for the entire sample.
    pub(crate) fn m5_policy_telemetry_word(&self, offset: usize) -> Result<u32> {
        if !self.m5 || offset & 3 != 0
            || offset.checked_add(4).is_none_or(|end| end > self.graph.policy_size) {
            return Err(EINVAL);
        }
        let at = self.graph.shared.policy.checked_add(offset).ok_or(EINVAL)?;
        Ok(self.object(self.bundle)?.word(at)?.load(Ordering::Relaxed))
    }

    /// Counts `submissions` in the firmware's submission history, in the slot of this moment.
    pub(crate) fn note_m5_submission_history(&self, submissions: u8) -> Result {
        if !self.m5 { return Err(EINVAL); }
        // The submission history is the M5 Pro build's status config +0x73c..; the common
        // 26A428 one (0x6d0 bytes) has no such table.
        if self.graph.is_common26() { return Ok(()); }
        let private = self.object(self.cluster)?;
        let base = m::M5_PRIVATE_LAYOUT.status_config;
        for _ in 0..64 {
            let generation = private.word(base + 0x74c)?.load(Ordering::Acquire);
            if generation & 1 != 0 { core::hint::spin_loop(); continue; }
            // The firmware moves the generation around every change of the geometry: while it
            // reads as it did, the geometry read under it stands, without four more loads.
            let (period, count) = match self.history_geometry.get() {
                Some((seen, period, count)) if seen == generation => (period, count),
                _ => {
                    let config = private.read_words::<4>(base + 0x73c)?;
                    fence(Ordering::Acquire);
                    if generation != private.word(base + 0x74c)?.load(Ordering::Acquire) {
                        core::hint::spin_loop(); continue;
                    }
                    let period = u64::from(config[0]) | u64::from(config[1]) << 32;
                    let count = u64::from(config[2]) | u64::from(config[3]) << 32;
                    self.history_geometry.set(Some((generation, period, count)));
                    (period, count)
                }
            };
            if period == 0 || count == 0 || count > 256 { return Err(EINVAL); }
            let (counter, frequency) = crate::g17::qos::clock();
            let ticks = crate::g17::qos::time_from_host(counter, frequency)?;
            // One slot per 25000 ticks of the period.
            let index = (ticks % period) / 25_000;
            if index >= count { return Err(ERANGE); }
            let byte = private.pointer(base + 0x750 + index as usize, 1)?;
            // SAFETY: retained, bounds-checked WC byte, written as a single byte
            // after reading the even, stable generation, as the firmware expects.
            unsafe {
                let value = core::ptr::read_volatile(byte).wrapping_add(submissions);
                core::ptr::write_volatile(byte, value);
            };
            fence(Ordering::SeqCst);
            return Ok(());
        }
        Err(EBUSY)
    }

    /// The address and size of a coprocessor's log buffer, for the message that publishes it.
    pub(in crate::g17) fn fw_log(role: Role) -> (u64, usize) {
        (FW_LOG[role as usize], FW_LOG_SIZE)
    }

    /// A coprocessor's log buffer, as the firmware has left it. Empty before the graph is built.
    pub(in crate::g17) fn fw_log_bytes(&self, role: Role) -> &[u8] {
        match self.fw_log_objects[role as usize].and_then(|index| self.object(index).ok()) {
            Some(object) => object.bytes(),
            None => &[],
        }
    }

    /// The hardware-data bundle as this driver built it, for diffing against a machine's own.
    pub(in crate::g17) fn hardware_data_bytes(&self) -> &[u8] {
        match self.object(self.bundle) {
            Ok(object) => object.bytes(),
            Err(_) => &[],
        }
    }

    pub(crate) fn build_m5(&mut self, dev: &AsahiDrmDevice, uat: &mmu::Uat,
        id: &crate::regs::G17Id, board: &m::Board) -> Result {
        use mmu::{PROT_FW_SHARED_RW as RW, PROT_FW_SHARED_RO as RO,
                  PROT_GPU_SHARED_RW as GPU};
        self.m5 = true;
        self.graph = board.graph;
        let common26 = board.graph.is_common26();
        // TA, fragment and compute share the compute work ring per priority; every
        // preparation's claim on it is counted from the first publication on.
        self.outer_credits = Some(super::outer_credit::Credits::new()?);
        let fixed = |address, size, protection| KernelObject::new(dev,
            uat.kernel_vm(), Placement::At(address), size, protection, CpuMap::WriteCombined);
        let fixed_controller = |address, size| {
            KernelObject::new(dev, uat.kernel_vm(), Placement::At(address), size,
                mmu::PROT_FW_PRIV_RW, CpuMap::WriteBack)
        };
        let private = board.graph.private;
        let shared = board.graph.shared;
        self.cluster = self.add(fixed(PRIVATE, private.total, RW)?)?;
        self.region_c = self.add(fixed(REGION_C, mmu::UAT_PGSZ, RO)?)?;
        self.roots = self.add(fixed(ROOTS, 0xc000, RO)?)?;
        let region_a = self.add(fixed(REGION_A, mmu::UAT_PGSZ, RO)?)?;
        self.add(fixed(FWCTL, mmu::UAT_PGSZ, RW)?)?;
        self.qos = self.add(fixed(QOS, m::G17P_QOS_RESOURCE_SIZE, RW)?)?;
        self.add(fixed(KICK_SHARED, m::M5_KICK_SHARED_SIZE, mmu::PROT_GPU_FW_SHARED_RW)?)?;
        // The J813's log buffers, which the firmware is told about once it runs (see
        // `publish_log_buffer`); it reads and writes them through its own kernel window.
        if common26 {
            for (role, address) in FW_LOG.into_iter().enumerate() {
                let object = self.add(fixed_controller(address, FW_LOG_SIZE)?)?;
                self.fw_log_objects[role] = Some(object);
            }
        }
        let aon_va = if common26 { COMMON26_AON } else { AON };
        let aon = self.add(fixed_controller(aon_va, m::M5_AON_PTD_SIZE)?)?;
        self.bundle = self.add(fixed_controller(BUNDLE, shared.total)?)?;
        self.pb = self.add(fixed(PB, mmu::UAT_PGSZ, RW)?)?;
        self.page_pool = self.add(fixed(UMA, mmu::UAT_PGSZ, RW)?)?;
        for stage in 0..2 {
            self.stamps[stage] = self.add(KernelObject::new(dev, uat.kernel_vm(),
                Placement::In(uat.kernel_dynamic_range(), mmu::UAT_PGSZ as u64),
                mmu::UAT_PGSZ, mmu::PROT_FW_SHARED_RW, CpuMap::WriteCombined)?)?;
        }
        let views = board.graph.profile.region_view_low_addrs();
        self.alias(self.pb, uat.kernel_lower_vm(), views[0], GPU)?;
        self.alias(self.page_pool, uat.kernel_lower_vm(), views[1], GPU)?;
        let (low, high) = if common26 {
            (COMMON26_COMPLETION_LOW, COMMON26_COMPLETION_HIGH)
        } else {
            (COMPLETION_LOW, COMPLETION_HIGH)
        };
        let mut completions = [m::G17PKsmCompletionAliases { low: 0, high: 0 }; m::M5_25G76_COMPLETION_QUEUE_COUNT];
        for (index, pair) in completions.iter_mut().enumerate() {
            pair.low = low + index as u64 * COMPLETION_STRIDE;
            pair.high = high + index as u64 * COMPLETION_STRIDE;
            // After initialization the host only reads completion records.
            // Match the standard G17 completion ring's CPU write-back mapping;
            // keep firmware/GPU aliases, size and lifetime unchanged.
            let backing = if index == 0
            {
                KernelObject::new(dev, uat.kernel_vm(), Placement::At(pair.high),
                    m::M5_25G76_COMPLETION_BACKING_SIZE, RW, CpuMap::WriteBack)?
            } else {
                fixed(pair.high, m::M5_25G76_COMPLETION_BACKING_SIZE, RW)?
            };
            let object = self.add(backing)?;
            *self.completion_rings.get_mut(index).ok_or(EINVAL)? = Some(object);
            self.alias(object, uat.kernel_lower_vm(), pair.low, GPU)?;
        }
        m::encode_common26_aon_ptd(board, &mut self.object_mut(aon)?.bytes_mut()[..m::M5_AON_PTD_SIZE])
            .map_err(|_| EINVAL)?;
        if common26 {
            m::encode_common26_board_region_c(
                &mut self.object_mut(self.region_c)?.bytes_mut()[..m::REGION_C_SIZE])
                .map_err(|_| EINVAL)?;
            m::encode_common26_private_cluster(PRIVATE, FWCTL,
                &mut self.object_mut(self.cluster)?.bytes_mut()[..private.total])
                .map_err(|_| EINVAL)?;
        } else {
            m::encode_m5_region_c(self.object_mut(self.region_c)?.bytes_mut()).map_err(|_| EINVAL)?;
            m::encode_m5_private_cluster(PRIVATE, FWCTL,
                &mut self.object_mut(self.cluster)?.bytes_mut()[..private.total]).map_err(|_| EINVAL)?;
        }
        let windows = board.register_windows();
        let windows = &windows[..board.register_window_count()];
        for (index, window) in windows.iter().enumerate() {
            let spec = board.register_window_spec(index).ok_or(EINVAL)?;
            let stride = (u64::from(window.size) + mmu::UAT_PGMSK as u64) & !(mmu::UAT_PGMSK as u64);
            for (instance, physical) in spec.instances.iter().copied().enumerate() {
                let offset = physical & mmu::UAT_PGMSK as u64;
                let address = window.device_va.checked_sub(offset).ok_or(EINVAL)?
                    + instance as u64 * stride;
                let size = (offset + u64::from(window.size) + mmu::UAT_PGMSK as u64)
                    & !(mmu::UAT_PGMSK as u64);
                self.aliases.push(uat.kernel_vm().map_io(dev.as_ref().as_ref(), address,
                    (physical & !(mmu::UAT_PGMSK as u64)).try_into()?, size.try_into()?,
                    mmu::PROT_FW_MMIO_RW)?, GFP_KERNEL)?;
            }
        }
        let addresses = m::SharedAddresses {
            region_a: self.object(region_a)?.gpu_va(), region_c: REGION_C,
            hw_data_bundle: BUNDLE, hw_data_bundle_alloc: shared.total,
        };
        let views = m::PrimaryRegionViews {
            sentinel_high: BASE + 0xc07e_0000, alias_high: [PB, UMA],
        };
        m::encode_m5_hw_data(board, completions, QOS, windows,
            &mut self.object_mut(self.bundle)?.bytes_mut()[..m::M5_HW_DATA_SIZE]).map_err(|_| EINVAL)?;
        m::set_m5_kick_shared(board,
            &mut self.object_mut(self.bundle)?.bytes_mut()[..m::M5_HW_DATA_SIZE], KICK_SHARED)
            .map_err(|_| EINVAL)?;
        let masked_cores: u32 = id.core_masks.iter().map(|mask| mask.count_ones()).sum();
        let enabled_cores = if masked_cores != 0 { masked_cores }
            else { id.num_clusters.checked_mul(id.num_cores).ok_or(EOVERFLOW)? };
        m::set_m5_enabled_cores(board,
            &mut self.object_mut(self.bundle)?.bytes_mut()[..m::M5_HW_DATA_SIZE], enabled_cores)
            .map_err(|_| EINVAL)?;
        let bindings = m::M5PolicyBindings {
            state: PRIVATE + private.policy_state as u64, aon_ptd: aon_va, boot_timestamp: 0,
        };
        if common26 {
            m::encode_common26_bundle_policy(board, bindings, self.object_mut(self.bundle)?.bytes_mut())
        } else {
            m::encode_m5_bundle_policy(bindings, self.object_mut(self.bundle)?.bytes_mut())
        }.map_err(|_| EINVAL)?;
        // The controller configuration applies where the part's leakage calibration matches the
        // one it was made for, or on a board whose configuration holds for every part.
        let calibrated = match board.calibration {
            None => true,
            Some(expected) => {
                let node = dev.as_ref().as_ref().of_node().ok_or(EIO)?;
                let calibration: Result<KVec<u32>> =
                    node.get_property(c_str!("apple,leakage-calibration-words"));
                calibration.as_ref().is_ok_and(|words| words.as_slice() == expected)
            }
        };
        if let Some(defaults) = board.controller_defaults.filter(|_| calibrated) {
            defaults(self.object_mut(self.bundle)?.bytes_mut()).map_err(|_| EINVAL)?;
            let maximum = self.object(self.bundle)?.word(shared.policy + m::M5_POLICY_MAXIMUM_POWER)?
                .load(Ordering::Relaxed);
            m::set_m5_region_maximum_power(self.object_mut(self.region_c)?.bytes_mut(), maximum)
                .map_err(|_| EINVAL)?;
        }
        if let Some(microseconds) = board.idle_standby_us {
            m::set_m5_region_idle_standby(self.object_mut(self.region_c)?.bytes_mut(), microseconds)
                .map_err(|_| EINVAL)?;
        }
        for (index, role) in [m::InstanceRole::Primary, m::InstanceRole::Secondary].into_iter().enumerate() {
            let instance = m::InstanceAddresses {
                root: ROOTS + index as u64 * 0x8000,
                main_config: BUNDLE + shared.main_config[index] as u64,
                status_a: PRIVATE + private.roles[index].status_a as u64,
                status_b: if index == 0 { PRIVATE + private.status_b as u64 } else { 0 },
                secondary_extras: if index == 0 { [0; 2] } else {
                    [PRIVATE + private.status_config as u64, PRIVATE + private.secondary_extra_1 as u64]
                },
            };
            let profile = board.graph.profile;
            let channels = m::derive_channel_table_for_profile(profile, role,
                PRIVATE + board.graph.state_grids[index] as u64,
                instance.status_a, instance.main_config, BUNDLE);
            let offset = shared.main_config[index];
            m::encode_main_config_for_profile(profile, role, &addresses, &instance,
                &channels, if index == 0 { Some(&views) } else { None },
                &mut self.object_mut(self.bundle)?.bytes_mut()[offset..offset + m::MAIN_CONFIG_SIZE])
                .map_err(|_| EINVAL)?;
            let offset = index * 0x8000;
            m::encode_root_for_profile(profile, role, &addresses, &instance,
                &mut self.object_mut(self.roots)?.bytes_mut()[offset..offset + m::M5_ROOT_SIZE])
                .map_err(|_| EINVAL)?;
        }
        fence(Ordering::SeqCst);
        dev_info!(dev.as_ref(), "M5 firmware graph ready: private {:#x}+{:#x}, shared {:#x}+{:#x}\n",
            PRIVATE, private.total, BUNDLE, shared.total);
        Ok(())
    }
}
