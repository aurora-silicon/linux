// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Build the extended M3 controller images from admitted device-tree inputs.
//! These are constructor inputs. Firmware owns subsequent controller outputs.

use super::{hw, initdata::PStateTables};
use crate::{f32, float::F32};
use kernel::prelude::*;

struct Image(KVec<u8>);
impl Image {
    fn new(size: usize) -> Result<Self> {
        let mut bytes = KVec::new();
        bytes.resize(size, 0, GFP_KERNEL)?;
        Ok(Self(bytes))
    }
    fn word(&mut self, offset: usize, value: u32) -> Result {
        self.0
            .get_mut(offset..offset.checked_add(4).ok_or(EOVERFLOW)?)
            .ok_or(ERANGE)?
            .copy_from_slice(&value.to_le_bytes());
        Ok(())
    }
    fn pointer(&mut self, offset: usize, value: u64) -> Result {
        self.0
            .get_mut(offset..offset.checked_add(8).ok_or(EOVERFLOW)?)
            .ok_or(ERANGE)?
            .copy_from_slice(&value.to_le_bytes());
        Ok(())
    }
    fn float(&mut self, offset: usize, value: F32) -> Result {
        let bits = value.to_bits();
        if bits & 0x7f80_0000 == 0x7f80_0000 {
            return Err(EINVAL);
        }
        self.word(offset, bits)
    }
    fn reciprocal(&mut self, negative: usize, positive: usize, periods: u32) -> Result {
        if periods == 0 {
            return Err(EINVAL);
        }
        let alpha = f32!(1.0) / F32::from(periods);
        self.float(negative, f32!(1.0) - alpha)?;
        self.float(positive, alpha)
    }
}

/// What the 26B123 (27.2) images need beyond the 26.6.2 build, from the machine.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Abi26B123 {
    /// Size of the host crash copy buffer (the boot loader's `apple,coredump-copy-buffer`).
    pub(crate) crash_copy_bytes: u32,
    /// The FastDie temperature target in degrees C (the boot loader's `apple,fast-die0-target`,
    /// the machine ADT's `gpu-fast-die0-target`).
    pub(crate) fast_die_target_c: u32,
}

pub(crate) struct PowerImages {
    pub(crate) hardware: KVec<u8>,
    pub(crate) performance: KVec<u8>,
    pub(crate) power: KVec<u8>,
    pub(crate) globals: KVec<u8>,
    pub(crate) maximum_khz: u32,
}

impl PowerImages {
    /// `sram_uv` follows the same OPP iteration order as PwrConfig::perf_states.
    /// No reference-bin voltage or experimental OPP-power estimate is substituted.
    pub(crate) fn new(
        pwr: &hw::PwrConfig,
        sram_uv: &[u32],
        sensor: u64,
        retention: u64,
        timestamp_base: u64,
        io_records: &[u8],
        v26b123: Option<Abi26B123>,
    ) -> Result<Self> {
        let table = PStateTables::new(pwr, true, None, false)?;
        let second = table.secondary.ok_or(EINVAL)?;
        if table.len != 9
            || table.max != 8
            || sram_uv.len() != pwr.perf_states.len()
            || pwr.perf_base_pstate == 0
            || pwr.perf_base_pstate > 8
            || !(1..=1000).contains(&pwr.power_sample_period)
            || pwr.pwr_min_duty_cycle > 100
            || pwr.avg_power_min_duty_cycle > 100
            || io_records.len() != super::m3_mmio_26::SLOTS * super::m3_mmio_26::RECORD_BYTES
            || [sensor, retention, timestamp_base]
                .iter()
                .any(|value| *value < 0xffff_fc00_0000_0000 || value & 7 != 0)
        {
            return Err(EINVAL);
        }
        let top = &pwr.perf_states[table.primary[8] as usize];
        let base = &pwr.perf_states[table.primary[pwr.perf_base_pstate as usize] as usize];
        if top.freq_hz != 1_338_000_000 || base.freq_hz >= top.freq_hz {
            return Err(EINVAL);
        }
        let period = pwr.power_sample_period;
        let ppm_periods = pwr.ppm_filter_time_constant_ms / period;
        let average_periods = pwr.avg_power_filter_tc_ms / period;
        if [
            ppm_periods,
            average_periods,
            pwr.pwr_filter_time_constant,
            pwr.perf_filter_time_constant,
            pwr.perf_filter_time_constant2,
            pwr.avg_power_target_filter_tc,
            pwr.se_filter_time_constant,
            pwr.se_filter_time_constant_1,
        ]
        .contains(&0)
        {
            return Err(EINVAL);
        }
        let clocks = |count: u32| {
            count
                .checked_mul(period)
                .and_then(|v| v.checked_mul(24_000))
                .ok_or(EOVERFLOW)
        };
        let dt = F32::from(period) / f32!(1000.0);
        let ceiling = 800;
        let base_units = pwr.perf_base_pstate * 100;
        // The profile controller budget is independent of the loader's experimental
        // OPP power model. It is not a demand floor or a replacement voltage table.
        let budget = 22_044u32;
        let mut main = Image::new(0x2658)?;
        let mut performance = Image::new(0x6578)?;
        let mut power = Image::new(0x670)?;
        let mut globals = Image::new(0xe48)?;

        main.pointer(0x28, timestamp_base)?;
        main.0[0x640..0xe88].copy_from_slice(io_records);
        main.pointer(0xe88, retention)?;
        main.word(0xe90, 0x8122)?;
        main.word(0xe94, 2)?;
        main.word(0xe9c, 4)?;
        main.word(0xea0, 1)?;
        main.word(0xea4, 1)?;
        main.word(0xeb8, 1)?;
        main.word(0xec0, 1)?;
        main.word(0xec8, 1)?;
        main.word(0xed0, 24_000)?;
        main.word(0xed4, 1)?;
        main.word(0xed8, period)?;
        main.word(0xee0, 1)?;
        main.word(0xee4, 1)?;
        main.word(0xee8, 1)?;
        main.word(0xf04, 31)?;
        main.pointer(0xf24, 4)?;
        main.word(0xf34, 1)?;
        // Firmware completion is polled through its normal bounded status path.
        main.word(0xf38, 0)?;
        main.word(0xf4c, 49)?;
        main.word(0xf6c, 1)?;
        main.word(0xf88, 10)?;
        main.word(0xf8c, 3)?;
        main.word(0xfac, 1)?;
        main.word(0xfb8, 22)?;
        main.word(0xfbc, 5)?;
        main.word(0xfc0, 10)?;
        main.word(0xfc4, 8)?;
        main.word(0x1cd8, 8)?;
        for state in 0..9 {
            let primary = table.primary[state] as usize;
            let secondary = second[state] as usize;
            let item = &pwr.perf_states[primary];
            let auxiliary = &pwr.perf_states[secondary];
            if item.volt_mv.len() != 1
                || auxiliary.volt_mv.len() != 1
                || !(100..=1100).contains(&item.volt_mv[0])
                || !(100..=1100).contains(&auxiliary.volt_mv[0])
                || !(100_000..=1_100_000).contains(&sram_uv[primary])
                || !(100_000..=1_100_000).contains(&sram_uv[secondary])
                || sram_uv[primary].div_ceil(1000) < item.volt_mv[0]
                || sram_uv[secondary].div_ceil(1000) < auxiliary.volt_mv[0]
            {
                return Err(EINVAL);
            }
            main.word(0xfc8 + state * 4, item.freq_hz / 1_000_000)?;
            main.word(0x1808 + state * 4, auxiliary.freq_hz / 1_000_000)?;
            main.float(0x1848 + state * 4, f32!(1.02))?;
            for lane in 0..16 {
                main.word(0x1008 + state * 64 + lane * 4, item.volt_mv[0])?;
                main.word(
                    0x1408 + state * 64 + lane * 4,
                    sram_uv[primary].div_ceil(1000),
                )?;
            }
            main.word(0x1cdc + state * 4, auxiliary.freq_hz / 1_000_000)?;
            if state != 0 {
                main.word(0x1d1c + state * 64, auxiliary.volt_mv[0])?;
            }
            main.word(0x211c + state * 64, sram_uv[secondary].div_ceil(1000))?;
            if state != 0 {
                // Profile indices for the admitted eight-state ladder.
                let core_profile = if state <= 3 { state } else { 2 * state - 3 };
                let auxiliary_profile = if state <= 4 { state } else { 2 * state - 4 };
                main.word(0x19c8 + state * 4, core_profile as u32)?;
                main.word(0x1a08 + state * 4, auxiliary_profile as u32)?;
                let boost = item.freq_hz.saturating_sub(base.freq_hz) / 1_000_000;
                main.word(
                    0x1908 + state * 4,
                    100 * boost / ((top.freq_hz - base.freq_hz) / 1_000_000),
                )?;
            }
        }
        // The native relative-power calibration of each running firmware state.
        // Encode only these semantic percentages, not an object-image overlay.
        for (state, percent) in [15, 25, 36, 46, 58, 75, 90, 100].into_iter().enumerate() {
            main.word(0x18c8 + (state + 1) * 4, percent)?;
        }
        main.word(0x2544, 4)?;
        main.word(0x2548, 6)?;
        main.word(0x254c, 3)?;
        main.word(0x2550, 7)?;
        main.word(0x2554, 7)?;
        main.word(0x2568, 10)?;
        main.word(0x2570, 1)?;
        main.word(0x258c, 0x160000)?;
        main.word(0x2594, 1)?;
        main.word(0x2598, 1)?;
        main.word(0x259c, 1)?;
        main.word(0x25a4, 1)?;
        main.word(0x25a8, 1)?;
        main.word(0x25ac, 1)?;
        main.0[0x25b4..0x25e4].fill(0xff);
        main.word(0x2604, 1)?;
        main.word(0x2634, 1)?;

        globals.word(0x24, 3000)?;
        globals.word(0x78, 1)?;
        globals.word(0x84, budget)?;
        for offset in [0x88, 0x8c, 0x98] {
            globals.word(offset, ceiling)?;
        }
        globals.word(0x998, 40)?;
        globals.word(0x99c, 10)?;
        globals.word(0x9a0, 250)?;
        globals.word(0x9b8, 2)?;
        globals.word(0x9bc, 40)?;
        globals.word(0x9c0, 5)?;
        globals.word(0x9c8, 40)?;
        globals.word(0x9cc, 50)?;
        globals.word(0xe28, 3)?;

        power.word(0x10, average_periods)?;
        power.float(0x14, pwr.avg_power_ki_only * dt)?;
        power.float(0x18, pwr.avg_power_kp)?;
        power.word(0x1c, pwr.avg_power_min_duty_cycle)?;
        power.word(0x20, pwr.avg_power_target_filter_tc)?;
        power.word(0xc8, 11_000)?;
        power.word(0xcc, 8_000)?;
        power.float(0xd8, pwr.fast_die0_proportional_gain)?;
        power.float(0xdc, pwr.fast_die0_integral_gain * dt)?;
        power.word(0x12c, budget)?;
        power.float(0x130, pwr.ppm_kp)?;
        power.float(0x134, pwr.ppm_ki * dt)?;
        power.word(0x14c, 1)?;

        performance.word(4, pwr.pwr_sample_period_aic_clks)?;
        performance.word(8, pwr.pwr_sample_period_aic_clks)?;
        performance.word(0x10, 4)?;
        performance.float(0x14, f32!(1.0))?;
        performance.word(0x2c, 1)?;
        performance.word(0x30, 1)?;
        // Leave the observed hardware state at zero until firmware writes it.
        for offset in [0x40, 0x80, 0xb78, 0xb80, 0xbc0] {
            performance.word(offset, base_units)?;
        }
        for offset in [0x44, 0x6c, 0x70, 0x84] {
            performance.word(offset, 1)?;
        }
        for offset in [
            0x48, 0x88, 0x9f4, 0x9f8, 0xa38, 0xaac, 0xaf0, 0xb7c, 0x221c, 0x22b4, 0x22b8, 0x22f8,
            0x2870, 0x29f8, 0x29fc, 0x2a3c, 0x2b88, 0x2c44, 0x2c88,
        ] {
            performance.word(offset, ceiling)?;
        }
        performance.word(0x50, 100)?;
        performance.word(0x60, 4)?;
        performance.float(0x64, f32!(1.0))?;
        performance.word(0x90, 100)?;
        performance.word(0x9b8, 625)?;
        performance.reciprocal(0x9c4, 0x9cc, pwr.pwr_filter_time_constant)?;
        performance.float(0x9d4, pwr.pwr_integral_gain)?;
        performance.float(0x9dc, F32::from(pwr.pwr_integral_min_clamp))?;
        performance.float(0x9e0, F32::from(budget))?;
        performance.float(0x9e4, pwr.pwr_proportional_gain)?;
        performance.float(0x9ec, -F32::from(ceiling) / F32::from(budget))?;
        for offset in [0x9f0, 0xaa8, 0x22b0] {
            performance.word(offset, pwr.pwr_min_duty_cycle)?;
        }
        for offset in [0xa04, 0xabc, 0x28c8, 0x28cc, 0x28d0, 0x2a08] {
            performance.word(offset, budget)?;
        }
        performance.word(0xa70, ppm_periods.checked_mul(4).ok_or(EOVERFLOW)?)?;
        performance.reciprocal(0xa7c, 0xa84, ppm_periods)?;
        performance.float(0xa8c, pwr.ppm_ki * dt)?;
        performance.float(0xa98, f32!(65536.0))?;
        performance.float(0xa9c, pwr.ppm_kp)?;
        performance.word(0xac8, pwr.ppm_filter_time_constant_ms)?;
        performance.word(0xad0, clocks(ppm_periods)?)?;
        performance.word(0xb28, pwr.perf_tgt_utilization)?;
        performance.word(0xb30, pwr.perf_boost_min_util)?;
        performance.word(0xb34, pwr.perf_boost_ce_step)?;
        performance.word(0xb38, pwr.perf_reset_iters)?;
        performance.word(0xb40, 6)?;
        performance.word(0xb44, 1)?;
        performance.word(0xb48, pwr.perf_filter_drop_threshold)?;
        performance.reciprocal(0xb4c, 0xb54, pwr.perf_filter_time_constant)?;
        performance.reciprocal(0xb50, 0xb58, pwr.perf_filter_time_constant2)?;
        performance.float(0xb5c, pwr.perf_integral_gain)?;
        performance.float(0xb60, pwr.perf_integral_gain2)?;
        performance.float(0xb64, F32::from(pwr.perf_integral_min_clamp))?;
        performance.float(0xb68, f32!(95.0))?;
        performance.float(0xb6c, pwr.perf_proportional_gain)?;
        performance.float(0xb70, pwr.perf_proportional_gain2)?;
        performance.float(0xb74, F32::from(8 - pwr.perf_base_pstate) / f32!(0.95))?;
        performance.word(0xb8c, pwr.perf_tgt_utilization)?;
        performance.float(0x21f0, f32!(65536.0))?;
        performance.float(0x21f4, F32::from(pwr.pwr_min_duty_cycle))?;
        performance.float(0x21f8, F32::from(ceiling))?;
        performance.word(0x21fc, 1338)?;
        performance.float(0x2218, f32!(100.0))?;
        performance.float(0x2224, f32!(0.8))?;
        performance.float(0x2228, f32!(0.2))?;
        performance.word(0x2268, 0x4248)?;
        performance.word(0x2270, 8_000)?;
        performance.word(0x2278, 4)?;
        performance.float(0x228c, f32!(1.0))?;
        performance.float(0x2294, pwr.fast_die0_integral_gain * dt)?;
        performance.float(0x22a0, f32!(65536.0))?;
        performance.float(0x22a4, pwr.fast_die0_proportional_gain)?;
        performance.word(0x22c4, 11_000)?;
        performance.word(0x28c0, 1)?;
        performance.reciprocal(0x28dc, 0x28e0, pwr.avg_power_target_filter_tc)?;
        performance.word(
            0x28e4,
            pwr.avg_power_target_filter_tc
                .checked_mul(4)
                .ok_or(EOVERFLOW)?,
        )?;
        performance.word(
            0x28e8,
            pwr.avg_power_target_filter_tc
                .checked_mul(period)
                .ok_or(EOVERFLOW)?,
        )?;
        performance.word(0x28ec, clocks(pwr.avg_power_target_filter_tc)?)?;
        performance.word(0x29bc, average_periods.checked_mul(4).ok_or(EOVERFLOW)?)?;
        performance.reciprocal(0x29c8, 0x29d0, average_periods)?;
        performance.float(0x29d8, pwr.avg_power_ki_only * dt)?;
        performance.float(0x29e4, f32!(65536.0))?;
        performance.float(0x29e8, pwr.avg_power_kp)?;
        performance.word(0x29f4, pwr.avg_power_min_duty_cycle)?;
        performance.float(0x2a04, F32::from(budget))?;
        performance.word(0x2a14, pwr.avg_power_filter_tc_ms)?;
        performance.word(0x2a1c, clocks(average_periods)?)?;
        performance.word(0x2b4c, 50)?;
        performance.word(0x2b50, 1)?;
        performance.reciprocal(0x2b58, 0x2b60, pwr.se_filter_time_constant)?;
        performance.reciprocal(0x2b5c, 0x2b64, pwr.se_filter_time_constant_1)?;
        performance.float(0x2b68, pwr.se_ki * dt)?;
        performance.float(0x2b6c, pwr.se_ki_1 * dt)?;
        performance.float(0x2b74, f32!(65536.0))?;
        performance.float(0x2b78, pwr.se_kp)?;
        performance.float(0x2b7c, pwr.se_kp_1)?;
        performance.word(0x2b84, 100)?;
        performance.word(0x2b8c, 100)?;
        performance.word(0x2b98, pwr.se_target)?;
        performance.word(
            0x2ba4,
            pwr.se_filter_time_constant
                .checked_mul(period)
                .ok_or(EOVERFLOW)?,
        )?;
        performance.word(
            0x2ba8,
            pwr.se_filter_time_constant_1
                .checked_mul(period)
                .ok_or(EOVERFLOW)?,
        )?;
        performance.word(0x2bac, clocks(pwr.se_filter_time_constant)?)?;
        performance.word(0x2bb4, clocks(pwr.se_filter_time_constant_1)?)?;
        performance.float(0x2b94, f32!(8000.0))?;
        for offset in [0x2bc0, 0x2bc4, 0x2c30] {
            performance.float(offset, f32!(65536.0))?;
        }
        performance.word(0x2be8, 8000)?;
        performance.word(0x2bec, 4)?;
        performance.word(0x2bf0, pwr.se_inactive_threshold)?;
        performance.word(0x2bf4, pwr.se_engagement_criteria as u32)?;
        performance.word(0x2bf8, 2)?;
        performance.word(0x2bfc, 4)?;
        performance.word(0x2c00, pwr.se_reset_criteria)?;
        performance.word(0x2c40, pwr.avg_power_min_duty_cycle)?;
        performance.word(0x2e2c, 4)?;
        performance.float(0x2e38, f32!(0.96875))?;
        performance.float(0x2e40, f32!(0.03125))?;
        performance.float(0x2e48, f32!(5.0))?;
        performance.float(0x2e54, f32!(65536.0))?;
        performance.float(0x2e58, f32!(15.0))?;
        performance.word(0x2e64, 700)?;
        for offset in [0x2e68, 0x2e6c, 0x2e78] {
            performance.word(offset, ceiling)?;
        }
        performance.word(0x2f08, 0x4248)?;
        performance.word(0x2f10, 125)?;
        performance.word(0x2f14, 1)?;
        performance.pointer(0x3b50, sensor)?;
        performance.word(0x3b58, 0x4b48)?;
        performance.word(0x3b60, 1)?;
        performance.word(0x62a8, 1)?;
        if let Some(abi) = v26b123 {
            return Self::relocate_26b123(abi, pwr, &main, &performance, &power, &globals,
                io_records, top.freq_hz / 1000);
        }
        Ok(Self {
            hardware: main.0,
            performance: performance.0,
            power: power.0,
            globals: globals.0,
            maximum_khz: top.freq_hz / 1000,
        })
    }

    /// The 26.6.2 images at their 26B123 (27.2) positions. The same objects and words, moved
    /// by the 26B123 runs (the InitData family G16 26B123 shares; bytes outside the runs stay
    /// zero), plus what 26B123 adds:
    /// - the register map at its 26B123 positions ([`super::m3_mmio_26::position_26b123`]);
    ///   positions without a 26.6.2 slot stay empty;
    /// - main +0xfa0 empty and +0xfa8 the host crash copy buffer size (the retention table moves
    ///   to the runtime pointers, see `m3_config`);
    /// - the four power-up words at the performance object +0x9c..+0xa8;
    /// - the power-control image (root +0xb8) grown to 0x698 bytes: the words the 26.6.2 build
    ///   writes there (+0x10..+0x14c: average power, FastDie, PPM, SE enable) keep their
    ///   positions, so they carry over;
    /// - the state word of the performance object's shared3 block (see below).
    #[allow(clippy::too_many_arguments)]
    fn relocate_26b123(
        abi: Abi26B123,
        pwr: &hw::PwrConfig,
        main: &Image,
        performance: &Image,
        power: &Image,
        globals: &Image,
        io_records: &[u8],
        maximum_khz: u32,
    ) -> Result<Self> {
        use super::m3_mmio_26::{position_26b123, RECORD_BYTES, SLOTS, TABLE_OFFSET};
        fn reloc(src: &[u8], runs: &[(usize, usize, usize)], size: usize) -> Result<Image> {
            let mut out = Image::new(size)?;
            for &(start, end, delta) in runs {
                out.0.get_mut(start + delta..end + delta).ok_or(ERANGE)?
                    .copy_from_slice(src.get(start..end).ok_or(ERANGE)?);
            }
            Ok(out)
        }
        let release_cc = pwr.fast_die0_release_temp.checked_mul(100).ok_or(EOVERFLOW)?;
        let target_cc = abi.fast_die_target_c.checked_mul(100).ok_or(EOVERFLOW)?;
        if abi.crash_copy_bytes == 0 || abi.crash_copy_bytes & 0xfff != 0
            || abi.crash_copy_bytes > 0x10_0000
            || target_cc <= release_cc
            || io_records.len() != SLOTS * RECORD_BYTES
        {
            return Err(EINVAL);
        }
        let mut hardware = reloc(&main.0, &[(0, TABLE_OFFSET, 0), (0xe90, 0xebc, 0x120),
            (0xec0, 0x25b4, 0x11c), (0x2600, 0x263c, 0x120), (0x263c, 0x2658, 0x124)], 0x2788)?;
        for slot in 0..SLOTS {
            let Some(position) = position_26b123(slot) else { continue };
            let at = TABLE_OFFSET + position * RECORD_BYTES;
            hardware.0[at..at + RECORD_BYTES]
                .copy_from_slice(&io_records[slot * RECORD_BYTES..(slot + 1) * RECORD_BYTES]);
        }
        hardware.0[0xfa8..0xfb0].copy_from_slice(&u64::from(abi.crash_copy_bytes).to_le_bytes());
        let mut performance = reloc(&performance.0, &[(0, 0x9c, 0), (0x9c, 0x28b0, 0x10),
            (0x28b0, 0x2d08, 0x28), (0x2f08, 0x3b50, 0x20), (0x3b50, 0x62c4, 0x48),
            (0x6318, 0x63cc, 0x28), (0x63cc, 0x6578, 0x6a8)], 0x6c48)?;
        // Power-up target words, 1 each as the G16 26B123 path sets them: with 0 the power-up
        // returns at once and the register restore runs on an unpowered GPU.
        for at in [0x9c, 0xa0, 0xa4, 0xa8] {
            performance.word(at, 1)?;
        }
        let globals = reloc(&globals.0, &[(0, 0x38, 0), (0x38, 0xe48, 4)], 0xe4c)?;
        // Two performance-object words this InitData family adds, 1 each as the G14 27.2 path
        // sets them in the same 0x6c48-byte object; a word the 26.6.2 build already fills keeps
        // its value.
        for at in [0x2f34, 0x6ac8] {
            if performance.0[at..at + 4] == [0; 4] {
                performance.word(at, 1)?;
            }
        }
        // Every word the 26.6.2 build writes in the power-control image lies below +0x150.
        let mut power = reloc(&power.0, &[(0, 0x150, 0)], 0x698)?;
        // The FastDie limiter's target and release temperatures (centi-degrees C) come from the
        // machine: the target from its ADT, the release from the device tree's
        // apple,fast-die0-release-temp as for every G15 part.
        power.word(0xc8, target_cc)?;
        power.word(0xcc, release_cc)?;
        // The performance object carries a shared3 block (`fw::initdata::raw::HwDataShared3`)
        // at +0x62ac; the G14 27.2 path fills its table at +0x62b8. Its state word (`unk_8`,
        // `HwConfig::shared3_unk`, a performance-state index) must not stay 0: with the block
        // all zero the firmware never takes the first device-control records. T8122 has no
        // shared3 table (`shared3_tab` is empty, as upstream), so the table stays empty and the
        // state word lies above every state: the DT state count.
        let above_top = u32::try_from(pwr.perf_states.len()).map_err(|_| EINVAL)?;
        performance.word(0x62ac + 8, above_top)?;
        Ok(Self {
            hardware: hardware.0,
            performance: performance.0,
            power: power.0,
            globals: globals.0,
            maximum_khz,
        })
    }
}
