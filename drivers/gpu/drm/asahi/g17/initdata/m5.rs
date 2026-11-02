// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The M5 (26A428) bootstrap graph: linux-aurora t6050-gpu's `build_m5` (g17/initdata/m5.rs) on
//! this tree's objects, built after both coprocessors published their kernel roots
//! ([`mmu::UatFirmware::PublishedRoots`]) instead of before an SPTM context bind, and with the
//! platform's values ([`m::M5Platform`]) in place of J714's. Nothing past the bootstrap: no
//! metrics alias, no J714 power-controller calibration, no diagnostics.

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
const KICK_SHARED: u64 = BASE + 0x1d_8000;
const BUNDLE: u64 = BASE + 0xc078_8000;
const COMPLETION_HIGH: u64 = BASE + 0xc080_0000;
const COMPLETION_LOW: u64 = 0x70_0184_8000;
const COMPLETION_STRIDE: u64 = 0x2_8000;
const PB: u64 = BASE + 0x15e_0000;
const UMA: u64 = BASE + 0x15e_8000;

impl InitData {
    /// Builds the M5 graph in the kernel window both roots now share.
    pub(in crate::g17) fn build_m5(
        &mut self,
        dev: &AsahiDrmDevice,
        uat: &mmu::Uat,
        platform: &m::M5Platform,
        enabled_cores: u32,
    ) -> Result {
        use mmu::{PROT_FW_SHARED_RO as RO, PROT_FW_SHARED_RW as RW, PROT_GPU_SHARED_RW as GPU};
        if uat.geometry().kernel_range().start != BASE {
            dev_err!(
                dev.as_ref(),
                "M5 graph: kernel window is not at {:#x}\n",
                BASE
            );
            return Err(EINVAL);
        }
        self.m5 = true;
        let fixed = |address, size, protection| {
            KernelObject::new(
                dev,
                uat.kernel_vm(),
                Placement::At(address),
                size,
                protection,
                CpuMap::WriteCombined,
            )
        };
        let private = m::M5_PRIVATE_LAYOUT;
        let shared = m::M5_SHARED_LAYOUT;
        self.cluster = self.add(fixed(PRIVATE, private.total, RW)?)?;
        self.region_c = self.add(fixed(REGION_C, mmu::UAT_PGSZ, RO)?)?;
        self.roots = self.add(fixed(ROOTS, 0xc000, RO)?)?;
        let region_a = self.add(fixed(REGION_A, mmu::UAT_PGSZ, RO)?)?;
        self.add(fixed(FWCTL, mmu::UAT_PGSZ, RW)?)?;
        self.qos = self.add(fixed(QOS, m::G17P_QOS_RESOURCE_SIZE, RW)?)?;
        self.add(fixed(
            KICK_SHARED,
            m::M5_KICK_SHARED_SIZE,
            mmu::PROT_GPU_FW_SHARED_RW,
        )?)?;
        let aon = self.add(fixed(AON, m::M5_AON_PTD_SIZE, RW)?)?;
        self.bundle = self.add(fixed(BUNDLE, shared.total, RW)?)?;
        self.pb = self.add(fixed(PB, mmu::UAT_PGSZ, RW)?)?;
        self.page_pool = self.add(fixed(UMA, mmu::UAT_PGSZ, RW)?)?;
        // The primary-view sentinel needs zeroed backing.
        self.add(fixed(BASE + 0xc07e_0000, mmu::UAT_PGSZ, RW)?)?;
        self.alias(
            self.pb,
            uat.kernel_lower_vm(),
            m::REGION_VIEW_LOW_ADDRS[0],
            GPU,
        )?;
        self.alias(
            self.page_pool,
            uat.kernel_lower_vm(),
            m::REGION_VIEW_LOW_ADDRS[1],
            GPU,
        )?;
        let mut completions =
            [m::G17PKsmCompletionAliases { low: 0, high: 0 }; m::M5_25G76_COMPLETION_QUEUE_COUNT];
        for (index, pair) in completions.iter_mut().enumerate() {
            pair.low = COMPLETION_LOW + index as u64 * COMPLETION_STRIDE;
            pair.high = COMPLETION_HIGH + index as u64 * COMPLETION_STRIDE;
            // After initialization the host only reads completion records.
            let object = self.add(KernelObject::new(
                dev,
                uat.kernel_vm(),
                Placement::At(pair.high),
                m::M5_25G76_COMPLETION_BACKING_SIZE,
                RW,
                if index == 0 {
                    CpuMap::WriteBack
                } else {
                    CpuMap::WriteCombined
                },
            )?)?;
            self.m5_completions[index] = object;
            self.alias(object, uat.kernel_lower_vm(), pair.low, GPU)?;
        }
        let bad = |what: &str, e: m::InitdataError| {
            dev_err!(
                dev.as_ref(),
                "M5 graph: {} encoding failed: {:?}\n",
                what,
                e
            );
            EINVAL
        };
        m::encode_common26_aon_ptd(
            &mut self.object_mut(aon)?.bytes_mut()[..m::M5_AON_PTD_SIZE],
            platform,
        )
        .map_err(|e| bad("AON", e))?;
        m::encode_m5_region_c(self.object_mut(self.region_c)?.bytes_mut())
            .map_err(|e| bad("region C", e))?;
        m::encode_m5_private_cluster(
            PRIVATE,
            FWCTL,
            &mut self.object_mut(self.cluster)?.bytes_mut()[..private.total],
        )
        .map_err(|e| bad("private cluster", e))?;
        let windows = m::m5_register_windows(platform.windows).map_err(|e| bad("windows", e))?;
        let windows = &windows[..platform.windows.len()];
        for (window, spec) in windows.iter().zip(platform.windows) {
            let stride =
                (u64::from(window.size) + mmu::UAT_PGMSK as u64) & !(mmu::UAT_PGMSK as u64);
            for (instance, physical) in spec.instances.iter().copied().enumerate() {
                let offset = physical & mmu::UAT_PGMSK as u64;
                let address =
                    window.device_va.checked_sub(offset).ok_or(EINVAL)? + instance as u64 * stride;
                let size = (offset + u64::from(window.size) + mmu::UAT_PGMSK as u64)
                    & !(mmu::UAT_PGMSK as u64);
                self.aliases.push(
                    uat.kernel_vm().map_io(
                        dev.as_ref().as_ref(),
                        address,
                        (physical & !(mmu::UAT_PGMSK as u64)).try_into()?,
                        size.try_into()?,
                        mmu::PROT_FW_MMIO_RW,
                    )?,
                    GFP_KERNEL,
                )?;
            }
        }
        let addresses = m::SharedAddresses {
            region_a: self.object(region_a)?.gpu_va(),
            region_c: REGION_C,
            hw_data_bundle: BUNDLE,
            hw_data_bundle_alloc: shared.total,
        };
        let views = m::PrimaryRegionViews {
            sentinel_high: BASE + 0xc07e_0000,
            alias_high: [PB, UMA],
        };
        let hw = &mut self.object_mut(self.bundle)?.bytes_mut()[..m::M5_HW_DATA_SIZE];
        m::encode_m5_hw_data(completions, QOS, windows, platform, hw)
            .map_err(|e| bad("HwData", e))?;
        m::set_m5_kick_shared(hw, KICK_SHARED).map_err(|e| bad("kick shared", e))?;
        m::set_m5_enabled_cores(hw, enabled_cores).map_err(|e| bad("enabled cores", e))?;
        m::encode_m5_bundle_policy(
            m::M5PolicyBindings {
                state: PRIVATE + private.policy_state as u64,
                aon_ptd: AON,
                boot_timestamp: 0,
            },
            self.object_mut(self.bundle)?.bytes_mut(),
        )
        .map_err(|e| bad("policy", e))?;
        for (index, role) in [m::InstanceRole::Primary, m::InstanceRole::Secondary]
            .into_iter()
            .enumerate()
        {
            let instance = m::InstanceAddresses {
                root: ROOTS + index as u64 * 0x8000,
                main_config: BUNDLE + shared.main_config[index] as u64,
                status_a: PRIVATE + private.roles[index].status_a as u64,
                status_b: if index == 0 {
                    PRIVATE + private.status_b as u64
                } else {
                    0
                },
                secondary_extras: if index == 0 {
                    [0; 2]
                } else {
                    [
                        PRIVATE + private.status_config as u64,
                        PRIVATE + private.secondary_extra_1 as u64,
                    ]
                },
            };
            let profile = m::RootProfile::J714G17S26A428;
            let channels = m::derive_channel_table_for_profile(
                profile,
                role,
                PRIVATE + m::M5_PRIVATE_STATE_GRIDS[index] as u64,
                instance.status_a,
                instance.main_config,
                BUNDLE,
            );
            let offset = shared.main_config[index];
            m::encode_main_config_for_profile(
                profile,
                role,
                &addresses,
                &instance,
                &channels,
                if index == 0 { Some(&views) } else { None },
                &mut self.object_mut(self.bundle)?.bytes_mut()
                    [offset..offset + m::MAIN_CONFIG_SIZE],
            )
            .map_err(|e| bad("main config", e))?;
            let offset = index * 0x8000;
            m::encode_root_for_profile(
                profile,
                role,
                &addresses,
                &instance,
                &mut self.object_mut(self.roots)?.bytes_mut()[offset..offset + m::M5_ROOT_SIZE],
            )
            .map_err(|e| bad("root", e))?;
        }
        fence(Ordering::SeqCst);
        dev_info!(
            dev.as_ref(),
            "M5 firmware graph ready: private {:#x}+{:#x}, shared {:#x}+{:#x}, {} windows\n",
            PRIVATE,
            private.total,
            BUNDLE,
            shared.total,
            windows.len()
        );
        Ok(())
    }
}
