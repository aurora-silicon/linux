// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-chip T6040 and T6041 calibration data supplied by the boot loader.

use super::fw::initdata::{Opp, PerfConfig, RegisterWindow, PAGE_SIZE, PERF_STATES};
use crate::driver::AsahiDrmDevice;
use kernel::{c_str, of, prelude::*, str::CStr};

/// Canonical AON temperature-source keys for four-cluster T6041 parts.
const T6041_AON_KEYS: [u32; 5] = [
    u32::from_be_bytes(*b"g0TR"),
    u32::from_be_bytes(*b"g1TR"),
    u32::from_be_bytes(*b"g2TR"),
    u32::from_be_bytes(*b"g3TR"),
    u32::from_be_bytes(*b"r0TR"),
];

/// Validated per-chip calibration supplied by the boot loader for two-cluster (T6040)
/// and four-cluster (T6041) GPUs.
pub(super) struct Calibration {
    pub(super) perf: PerfConfig,
    pub(super) leakage: [u32; 56],
    pub(super) max_power_mw: u32,
    pub(super) active_mcc_mask: Option<u32>,
    pub(super) aon_table: Option<[u32; 15]>,
    clusters: usize,
    core_mask: u64,
    base: usize,
}

fn words<const N: usize>(node: &of::Node, name: &CStr) -> Result<[u32; N]> {
    let values: KVec<u32> = node.get_property(name)?;
    values.as_slice().try_into().map_err(|_| EINVAL)
}

// Raw ADT OPP slots and SGX register slots number 16. The supported firmware consumes
// the 11 populated selectors; the remaining handoff entries must all be zero.
const RAW_STATES: usize = 16;

fn active(values: [u32; RAW_STATES]) -> Result<[u32; PERF_STATES]> {
    if values[PERF_STATES..].iter().any(|&v| v != 0) {
        return Err(EINVAL);
    }
    values[..PERF_STATES].try_into().map_err(|_| EINVAL)
}

fn map(values: [u32; RAW_STATES]) -> Result<[u8; PERF_STATES]> {
    let values = active(values)?;
    if values[0] != 0
        || values[PERF_STATES - 1] != RAW_STATES as u32 - 1
        || values.iter().any(|&v| v >= RAW_STATES as u32)
        || values.windows(2).any(|p| p[0] >= p[1])
    {
        return Err(EINVAL);
    }
    Ok(values.map(|v| v as u8))
}

fn relative(values: &[u32; PERF_STATES]) -> bool {
    values[0] == 0
        && values[PERF_STATES - 1] == 100
        && values.windows(2).all(|p| p[0] <= p[1] && p[1] <= 100)
}

fn check_aon_keys(table: &[u32; 15]) -> bool {
    T6041_AON_KEYS
        .iter()
        .enumerate()
        .all(|(i, &key)| table[i * 3] == key)
}

fn check_perf_opps(
    opps: &[Opp],
    cells: usize,
    states_a: &[u8; PERF_STATES],
    states_b: &[u8; PERF_STATES],
    relative_b: &[u32; PERF_STATES],
    base: usize,
) -> Result {
    if opps.len() != RAW_STATES || opps[0].freq_hz != 0 || opps.iter().any(|opp| opp.cells != cells)
    {
        return Err(EINVAL);
    }
    for map in [states_a, states_b] {
        let mut previous = 0;
        for &index in &map[1..] {
            let opp = &opps[usize::from(index)];
            let mhz = opp.freq_hz / 1_000_000;
            if mhz <= previous
                || opp.volt_uv[..cells]
                    .iter()
                    .any(|v| !(400_000..=1_300_000).contains(v))
            {
                return Err(EINVAL);
            }
            previous = mhz;
        }
    }
    let base_freq = opps[usize::from(states_a[base])].freq_hz / 1_000_000;
    let top_freq = opps[usize::from(states_a[PERF_STATES - 1])].freq_hz / 1_000_000;
    for (i, &index) in states_a.iter().enumerate() {
        let freq = opps[usize::from(index)].freq_hz / 1_000_000;
        let expected = if i < base {
            0
        } else {
            100 * (freq - base_freq) / (top_freq - base_freq)
        };
        if u64::from(relative_b[i]) != expected {
            return Err(EINVAL);
        }
    }
    Ok(())
}

impl Calibration {
    /// Read mandatory v2 calibration for a `clusters`-cluster GPU (2 for T6040, 4 for T6041).
    pub(super) fn read(dev: &AsahiDrmDevice, clusters: u32) -> Result<Self> {
        Self::read_opt(dev, clusters)?.ok_or(EINVAL)
    }

    /// Read optional v2 calibration for a `clusters`-cluster GPU.
    ///
    /// Returns `Ok(None)` when the handoff version property is absent or is a
    /// v1 handoff (`1`, which omits leakage and power ceiling — allowing
    /// profiles with measured static data, such as 40-core T6041, to keep
    /// booting on `aura` v1 loaders while mandatory [`Self::read`] still fails
    /// closed). For v2 (`2`), every required calibration property must be
    /// present and valid; any other version returns `EINVAL`.
    pub(super) fn read_opt(dev: &AsahiDrmDevice, clusters: u32) -> Result<Option<Self>> {
        let node = dev.as_ref().as_ref().of_node().ok_or(EIO)?;
        let clusters = usize::try_from(clusters)?;
        let (version_prop, max_power_ceiling) = match clusters {
            2 => (c_str!("apple,t6040-handoff-version"), 200_000u32),
            4 => (c_str!("apple,t6041-handoff-version"), 250_000u32),
            _ => return Err(EINVAL),
        };
        let Some(version) = node.get_opt_property::<u32>(version_prop)? else {
            return Ok(None);
        };
        match version {
            1 => return Ok(None),
            2 => {}
            _ => return Err(EINVAL),
        }
        let core_mask: u64 = match clusters {
            2 => {
                let mask: u32 = node.get_property(c_str!("apple,core-mask"))?;
                if mask == 0 || mask & !0xfffff != 0 {
                    return Err(EINVAL);
                }
                u64::from(mask)
            }
            4 => {
                let mask: u64 = node.get_property(c_str!("apple,core-mask"))?;
                if mask == 0 || mask & !((1u64 << 40) - 1) != 0 {
                    return Err(EINVAL);
                }
                mask
            }
            _ => return Err(EINVAL),
        };
        let max_power_mw: u32 = node.get_property(c_str!("apple,max-power-mw"))?;
        let base = node.get_property::<u32>(c_str!("apple,perf-base-pstate"))? as usize;
        if !(1..=max_power_ceiling).contains(&max_power_mw) || base != 1 {
            return Err(EINVAL);
        }
        let perf = PerfConfig {
            states_a: map(words(&node, c_str!("apple,perf-map-a"))?)?,
            states_b: map(words(&node, c_str!("apple,perf-map-b"))?)?,
            relative_a: active(words(&node, c_str!("apple,perf-relative-a"))?)?,
            relative_b: active(words(&node, c_str!("apple,perf-relative-b"))?)?,
            sram_min_mv: 810,
        };
        if !relative(&perf.relative_a)
            || !relative(&perf.relative_b)
            || perf.relative_b[..=base].iter().any(|&v| v != 0)
        {
            return Err(EINVAL);
        }
        let core: KVec<u32> = node.get_property(c_str!("apple,core-leak-coef"))?;
        let sram: KVec<u32> = node.get_property(c_str!("apple,sram-leak-coef"))?;
        if core.len() != clusters || sram.len() != clusters {
            return Err(EINVAL);
        }
        let cs = words::<1>(&node, c_str!("apple,cs-leak-coef"))?;
        let afr = words::<1>(&node, c_str!("apple,afr-leak-coef"))?;
        // Positive finite binary32 values. Integer comparisons preserve their ordering.
        if Iterator::chain(
            Iterator::chain(Iterator::chain(core.iter(), sram.iter()), &cs),
            &afr,
        )
        .any(|&v| !(0x3f80_0000..0x4780_0000).contains(&v))
        {
            return Err(EINVAL);
        }
        let mut leakage = [0; 56];
        leakage[..clusters].copy_from_slice(&core);
        leakage[32..32 + clusters].copy_from_slice(&sram);
        leakage[52] = cs[0];
        leakage[54] = afr[0];

        let (active_mcc_mask, aon_table) = if clusters == 4 {
            let mask: u32 = node.get_property(c_str!("apple,active-mcc-bit-vector"))?;
            if mask == 0 || mask & !0xff != 0 {
                return Err(EINVAL);
            }
            let table = words::<15>(&node, c_str!("apple,aon-table"))?;
            if !check_aon_keys(&table) {
                return Err(EINVAL);
            }
            (Some(mask), Some(table))
        } else {
            (None, None)
        };

        Ok(Some(Self {
            perf,
            leakage,
            max_power_mw,
            active_mcc_mask,
            aon_table,
            clusters,
            core_mask,
            base,
        }))
    }

    /// Check the loader's mask against the GPU clusters before starting its firmware.
    pub(super) fn check_cores(&self, masks: &[u32]) -> Result {
        if masks.len() != self.clusters {
            return Err(EINVAL);
        }
        for (index, &mask) in masks.iter().enumerate() {
            let expected = ((self.core_mask >> (index * 10)) & 0x3ff) as u32;
            if mask != expected {
                return Err(EINVAL);
            }
        }
        Ok(())
    }

    /// Set register-map slot 3's instance count directly from `apple,active-mcc-bit-vector`
    /// and shift subsequent window virtual addresses to keep the aperture contiguous.
    pub(super) fn apply_mcc(&self, windows: &mut [RegisterWindow]) -> Result {
        let Some(mask) = self.active_mcc_mask else {
            return Ok(());
        };
        let instances = mask.count_ones();
        let mut slot3_index = None;
        for (index, window) in windows.iter().enumerate() {
            if window.slot == 3 {
                if slot3_index.is_some() {
                    return Err(EINVAL);
                }
                slot3_index = Some(index);
            }
        }
        let Some(slot3_index) = slot3_index else {
            return Err(EINVAL);
        };
        let slot3 = &mut windows[slot3_index];
        let old_instances = slot3.instances;
        let page_mask = PAGE_SIZE as u64 - 1;
        let page_offset = slot3.phys & page_mask;
        let per_instance = page_offset
            .checked_add(u64::from(slot3.size))
            .and_then(|v| v.checked_add(page_mask))
            .ok_or(EINVAL)?
            & !page_mask;
        slot3.instances = instances;
        if slot3.mapping(instances - 1).is_none() {
            return Err(EINVAL);
        }
        let slot3_end = slot3
            .va
            .checked_add(
                u64::from(instances)
                    .checked_mul(per_instance)
                    .ok_or(EINVAL)?,
            )
            .ok_or(EINVAL)?;
        if instances >= old_instances {
            let delta = u64::from(instances - old_instances)
                .checked_mul(per_instance)
                .ok_or(EINVAL)?;
            for window in &mut windows[slot3_index + 1..] {
                window.va = window.va.checked_add(delta).ok_or(EINVAL)?;
                if window.va < slot3_end {
                    return Err(EINVAL);
                }
            }
        } else {
            let delta = u64::from(old_instances - instances)
                .checked_mul(per_instance)
                .ok_or(EINVAL)?;
            for window in &mut windows[slot3_index + 1..] {
                window.va = window.va.checked_sub(delta).ok_or(EINVAL)?;
                if window.va < slot3_end {
                    return Err(EINVAL);
                }
            }
        }
        Ok(())
    }

    /// The performance maps index OPPs in ADT order, rather than sorted frequency order.
    pub(super) fn check_opps(&self, opps: &[Opp]) -> Result {
        check_perf_opps(
            opps,
            self.clusters,
            &self.perf.states_a,
            &self.perf.states_b,
            &self.perf.relative_b,
            self.base,
        )
    }
}

#[kunit_tests(asahi_t6040_calibration)]
mod tests {
    use super::*;

    fn chip() -> Calibration {
        Calibration {
            perf: PerfConfig {
                states_a: map([0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15, 0, 0, 0, 0, 0]).unwrap(),
                states_b: map([0, 1, 2, 3, 4, 6, 7, 9, 11, 13, 15, 0, 0, 0, 0, 0]).unwrap(),
                relative_a: [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100],
                relative_b: [0, 0, 7, 14, 21, 35, 50, 64, 78, 92, 100],
                sram_min_mv: 810,
            },
            leakage: [0; 56],
            max_power_mw: 70_000,
            active_mcc_mask: None,
            aon_table: None,
            clusters: 2,
            core_mask: 0xfffff,
            base: 1,
        }
    }

    #[test]
    fn operating_points_and_mask_must_match_the_handoff() {
        let chip = chip();
        let mut opps = [Opp {
            freq_hz: 0,
            volt_uv: [800_000; super::super::fw::initdata::OPP_CELLS],
            cells: 2,
        }; 16];
        for (i, opp) in opps.iter_mut().enumerate() {
            opp.freq_hz = i as u64 * 100_000_000;
        }
        assert!(chip.check_opps(&opps).is_ok());
        opps[0].freq_hz = 100_000_000;
        assert!(chip.check_opps(&opps).is_err());
        opps[0].freq_hz = 0;
        assert!(chip.check_cores(&[0x3ff, 0x3ff]).is_ok());
        assert!(chip.check_cores(&[0x3ff, 0x1ff]).is_err());
        assert!(chip.check_cores(&[0xfffff]).is_err());
        // Both eleven-state maps must reach raw OPP 15, including its second voltage lane.
        assert_eq!(chip.perf.states_a[PERF_STATES - 1], 15);
        assert_eq!(chip.perf.states_b[PERF_STATES - 1], 15);
        opps[15].volt_uv[1] = 0;
        assert!(chip.check_opps(&opps).is_err());
        opps[15].volt_uv[1] = 800_000;
        assert!(chip.check_opps(&opps).is_ok());
        opps[6].volt_uv[1] = 0;
        assert!(chip.check_opps(&opps).is_err());
        opps[6].volt_uv[1] = 800_000;
        opps[6].freq_hz = opps[4].freq_hz;
        assert!(chip.check_opps(&opps).is_err());
        opps[6].freq_hz = 601_000_000;
        assert!(chip.check_opps(&opps).is_ok());
        opps[6].freq_hz = 611_000_000;
        assert!(chip.check_opps(&opps).is_err());
    }

    #[test]
    fn maps_refuse_off_repeats_and_out_of_range() {
        let valid = [0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15, 0, 0, 0, 0, 0];
        assert!(map(valid).is_ok());
        for (index, value) in [
            (0, 1),
            (1, 0),
            (5, 4),
            (10, 16),
            (10, u32::MAX),
            (10, 14),
            (11, 1),
            (15, 15),
        ] {
            let mut bad = valid;
            bad[index] = value;
            assert!(map(bad).is_err());
        }
    }

    #[test]
    fn relative_tables_are_bounded_and_monotonic() {
        let mut values = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100];
        assert!(relative(&values));
        values[5] = 101;
        assert!(!relative(&values));
        values[5] = 39;
        assert!(!relative(&values));
    }

    #[test]
    fn t6041_handoff_validates_cores_mcc_opps_and_aon_keys() {
        let aon_table = [
            u32::from_be_bytes(*b"g0TR"),
            0x00c2_0001,
            0x0000_1401,
            u32::from_be_bytes(*b"g1TR"),
            0x0097_0001,
            0x0000_1401,
            u32::from_be_bytes(*b"g2TR"),
            0x0092_0001,
            0x0000_1401,
            u32::from_be_bytes(*b"g3TR"),
            0x00c7_0001,
            0x0000_1401,
            u32::from_be_bytes(*b"r0TR"),
            0x012c_0001,
            0x0000_1401,
        ];
        assert!(check_aon_keys(&aon_table));
        let mut bad_aon = aon_table;
        bad_aon[12] = u32::from_be_bytes(*b"x0TR");
        assert!(!check_aon_keys(&bad_aon));

        let mut cal = Calibration {
            perf: PerfConfig {
                states_a: map([0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15, 0, 0, 0, 0, 0]).unwrap(),
                states_b: map([0, 1, 2, 3, 4, 5, 7, 9, 11, 13, 15, 0, 0, 0, 0, 0]).unwrap(),
                relative_a: [0, 10, 18, 25, 31, 40, 50, 62, 70, 78, 100],
                relative_b: [0, 0, 7, 14, 21, 35, 50, 64, 78, 92, 100],
                sram_min_mv: 810,
            },
            leakage: [0; 56],
            max_power_mw: 110_000,
            active_mcc_mask: Some(0x3f),
            aon_table: Some(aon_table),
            clusters: 4,
            core_mask: 0xf6_fdbf_6fdb,
            base: 1,
        };
        assert!(cal.check_cores(&[0x3db, 0x3db, 0x3db, 0x3db]).is_ok());
        assert!(cal.check_cores(&[0x3ff, 0x3ff, 0x3ff, 0x3ff]).is_err());
        assert!(cal.check_cores(&[0x3db, 0x3db]).is_err());

        let base_windows = [
            RegisterWindow {
                slot: 0,
                phys: 0x5_0241_4000,
                va: 0xffff_fc21_8020_0000,
                size: 0x4000,
                unk_18: 0,
                unk_20: 2,
                instances: 1,
                phys_stride: 0,
            },
            RegisterWindow {
                slot: 3,
                phys: 0x2_201c_4000,
                va: 0xffff_fc21_8020_8000,
                size: 0x1_8000,
                unk_18: 0,
                unk_20: 2,
                instances: 8,
                phys_stride: 0x200_0000,
            },
            RegisterWindow {
                slot: 4,
                phys: 0x5_0244_c000,
                va: 0xffff_fc21_802c_c000,
                size: 0x4000,
                unk_18: 0,
                unk_20: 0,
                instances: 1,
                phys_stride: 0,
            },
        ];
        // Shrinking slot 3 from 8 to 6 MCCs updates instances and shifts slot 4 down by 0x30000.
        let mut wins = base_windows;
        assert!(cal.apply_mcc(&mut wins).is_ok());
        assert_eq!(wins[0].va, 0xffff_fc21_8020_0000);
        assert_eq!(wins[1].instances, 6);
        assert_eq!(wins[2].va, 0xffff_fc21_8029_c000);

        // Growing slot 3 back from 6 to 8 MCCs restores the original 8-MCC aperture layout.
        cal.active_mcc_mask = Some(0xff);
        assert!(cal.apply_mcc(&mut wins).is_ok());
        assert_eq!(wins[1].instances, 8);
        assert_eq!(wins[2].va, 0xffff_fc21_802c_c000);

        // Missing or duplicate slot 3 fails closed.
        let mut empty: [RegisterWindow; 0] = [];
        assert!(cal.apply_mcc(&mut empty).is_err());
        let mut dup = [base_windows[1], base_windows[1]];
        assert!(cal.apply_mcc(&mut dup).is_err());

        let mut opps = [Opp {
            freq_hz: 0,
            volt_uv: [800_000; super::super::fw::initdata::OPP_CELLS],
            cells: 4,
        }; 16];
        for (i, opp) in opps.iter_mut().enumerate() {
            opp.freq_hz = i as u64 * 100_000_000;
        }
        assert!(cal.check_opps(&opps).is_ok());
        opps[15].volt_uv[3] = 0;
        assert!(cal.check_opps(&opps).is_err());
        opps[15].volt_uv[3] = 800_000;
        opps[0].cells = 2;
        assert!(cal.check_opps(&opps).is_err());
    }
}
