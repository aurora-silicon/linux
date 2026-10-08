// SPDX-License-Identifier: GPL-2.0-only OR MIT
use kernel::{c_str, prelude::*};
use crate::{driver, float::F32, g16_config::layout::Object, f32};

pub(crate) fn inputs(dev: &driver::AsahiDevice) -> Result<KVec<(Object, usize, u32)>> {
    use Object::{Power, PowerPerformance as PP, Globals, Main};
    let node = dev.as_ref().of_node().ok_or(ENODEV)?;
    let board = crate::g16_board::get()?;
    let max_power = board.max_power_mw;
    if max_power == 0 { return Err(EINVAL); }
    macro_rules! u { ($name:literal) => { node.get_property::<u32>(c_str!($name))? }; }
    macro_rules! f { ($name:literal) => { node.get_property::<F32>(c_str!($name))? }; }
    let period = u!("apple,power-sample-period");
    let base = u!("apple,perf-base-pstate").min(crate::g16_power::requested_state()?);
    let max = crate::g16_power::requested_state()?;
    let duty = u!("apple,pwr-min-duty-cycle");
    // The 26 firmware controller has no qualified target/release mapping.
    let thermal: Option<(u32, u32)> = None;
    let (target, release) = thermal.unwrap_or((0, 0));
    let avg_duty = u!("apple,avg-power-min-duty-cycle");
    let pwr_tc = u!("apple,pwr-filter-time-constant");
    let ppm_ms = u!("apple,ppm-filter-time-constant-ms");
    let perf_tc = u!("apple,perf-filter-time-constant");
    let perf_tc2 = u!("apple,perf-filter-time-constant2");
    let avg_ms = u!("apple,avg-power-filter-tc-ms");
    let avg_target_tc = u!("apple,avg-power-target-filter-tc");
    let se_tc = u!("apple,se-filter-time-constant");
    let se_tc1 = u!("apple,se-filter-time-constant-1");
    if period == 0 || period > 1000 || ppm_ms < period || avg_ms < period ||
        [pwr_tc, perf_tc, perf_tc2, avg_target_tc, se_tc, se_tc1].contains(&0) ||
        base == 0 || base > max || duty > 100 || avg_duty > 100 { return Err(EINVAL); }
    let dt = F32::from(period) / f32!(1000.0);
    let one = f32!(1.0);
    let pwr_a = one / F32::from(pwr_tc);
    let ppm_periods = ppm_ms / period;
    let ppm_a = one / F32::from(ppm_periods);
    let perf_a = one / F32::from(perf_tc);
    let perf_a2 = one / F32::from(perf_tc2);
    let avg_periods = avg_ms / period;
    let avg_a = one / F32::from(avg_periods);
    let avg_target_a = one / F32::from(avg_target_tc);
    let se_a = one / F32::from(se_tc);
    let se_a1 = one / F32::from(se_tc1);
    let ppm_ki = f!("apple,ppm-ki") * dt;
    let ppm_kp = f!("apple,ppm-kp");
    let avg_ki = f!("apple,avg-power-ki-only") * dt;
    let avg_kp = f!("apple,avg-power-kp");
    let fast_ki = if thermal.is_some() { f!("apple,fast-die0-integral-gain") * dt } else { f32!(0.0) };
    let fast_kp = if thermal.is_some() { f!("apple,fast-die0-proportional-gain") } else { f32!(0.0) };
    let perf_ki = f!("apple,perf-integral-gain");
    let perf_ki2 = f!("apple,perf-integral-gain2");
    let perf_kp = f!("apple,perf-proportional-gain");
    let perf_kp2 = f!("apple,perf-proportional-gain2");
    let util = u!("apple,perf-tgt-utilization");
    let perf_reset: u32 = node.get_property(c_str!("apple,perf-reset-iters")).unwrap_or(6);
    let mut words = KVec::new();
    macro_rules! w { ($owner:expr, $offset:expr, $value:expr) => { words.push(($owner, $offset, $value), GFP_KERNEL)? }; }
    macro_rules! wf { ($owner:expr, $offset:expr, $value:expr) => {{
        let value: F32 = $value;
        // SAFETY: F32 is repr(transparent) over u32; this copies its IEEE bits.
        w!($owner, $offset, unsafe { core::mem::transmute::<F32, u32>(value) });
    }}; }
    for o in [0xaf0, 0x22f8, 0x2870, 0x2a3c, 0x2c88] { w!(PP,o,max*100); }
    for (o,v) in [(0xc8,target),(0xcc,release),(0x10,avg_periods),(0x1c,avg_duty),
        (0x20,avg_target_tc),(0x12c,max_power),(0x14c,1)] { w!(Power,o,v); }
    wf!(Power,0x14,avg_ki); wf!(Power,0x18,avg_kp);
    wf!(Power,0xd8,fast_kp); wf!(Power,0xdc,fast_ki);
    wf!(Power,0x130,ppm_kp); wf!(Power,0x134,ppm_ki);
    w!(Globals,0x84,max_power);
    w!(Main,0xed8,period); w!(PP,0x4,u!("apple,pwr-sample-period-aic-clks"));
    w!(PP,0x8,u!("apple,pwr-sample-period-aic-clks")); w!(PP,0x62a8,1);
    for (o,v) in [(0x40,base*100),(0x44,1),(0x48,max*100),(0x50,100),
        (0x60,4),(0x6c,1),(0x70,1),(0x80,base*100),(0x84,1),(0x88,max*100),(0x90,100),
        (0x9b8,625),(0x9f0,duty),(0x9f4,max*100),(0x9f8,max*100),(0xa04,max_power),(0xa38,max*100),
        (0xa70,ppm_periods*4),(0xaa8,duty),(0xaac,max*100),(0xabc,max_power),
        (0xac8,ppm_ms),(0xad0,ppm_periods*period*24000),
        (0xb28,util),(0xb30,u!("apple,perf-boost-min-util")),
        (0xb34,u!("apple,perf-boost-ce-step")),(0xb38,perf_reset),
        (0xb40,6),(0xb44,1),(0xb48,u!("apple,perf-filter-drop-threshold")),
        (0xb78,base*100),(0xb7c,max*100),(0xb80,base*100),(0xb8c,util),(0xbc0,base*100),
        (0x21fc,crate::g16_board::get()?.frequencies_mhz[max as usize]),
        (0x221c,max*100),
        (0x2270,release),(0x22b0,duty),(0x22b4,max*100),(0x22b8,max*100),(0x22c4,target),
        (0x28c0,1),(0x28c8,max_power),(0x28cc,max_power),(0x28d0,max_power),
        (0x28e4,avg_target_tc*4),(0x28e8,period*avg_target_tc),(0x28ec,period*avg_target_tc*24000),
        (0x29bc,avg_periods*4),(0x29f4,avg_duty),(0x29f8,max*100),(0x29fc,max*100),
        (0x2a08,max_power),(0x2a14,avg_ms),(0x2a1c,avg_periods*period*24000),
        (0x2b4c,50),(0x2b50,1),(0x2b84,100),(0x2b88,max*100),(0x2b8c,100),
        (0x2b98,u!("apple,se-target")),(0x2ba4,se_tc*period),(0x2ba8,se_tc1*period),
        (0x2bac,se_tc*period*24000),(0x2bb4,se_tc1*period*24000),
        (0x2be8,release),(0x2bec,4),(0x2bf0,u!("apple,se-inactive-threshold")),
        (0x2bf4,u!("apple,se-engagement-criteria")),(0x2bf8,2),(0x2bfc,4),
        (0x2c00,u!("apple,se-reset-criteria")),(0x2c40,avg_duty),(0x2c44,max*100)] { w!(PP,o,v); }
    wf!(PP,0x64,one);
    for (o,v) in [(0x9c4,one-pwr_a),(0x9cc,pwr_a),(0x9d4,f!("apple,pwr-integral-gain")),
        (0x9dc,F32::from(u!("apple,pwr-integral-min-clamp"))), (0x9e0,F32::from(max_power)),
        (0x9e4,f!("apple,pwr-proportional-gain")),(0x9ec,-F32::from(max*100)/F32::from(max_power)),
        (0xa7c,one-ppm_a),(0xa84,ppm_a),(0xa8c,ppm_ki),(0xa98,f32!(65536.0)),(0xa9c,ppm_kp),
        (0xb4c,one-perf_a),(0xb50,one-perf_a2),(0xb54,perf_a),(0xb58,perf_a2),
        (0xb5c,perf_ki),(0xb60,perf_ki2),(0xb64,F32::from(u!("apple,perf-integral-min-clamp"))),
        (0xb68,f32!(95.0)),(0xb6c,perf_kp),(0xb70,perf_kp2),
        (0xb74,F32::from(max-base)/f32!(0.95)),
        (0x21f0,f32!(65536.0)),(0x21f4,F32::from(duty)),(0x21f8,F32::from(max*100)),
        (0x2218,f32!(100.0)),(0x2294,fast_ki),(0x22a4,fast_kp),
        (0x28dc,one-avg_target_a),(0x28e0,avg_target_a),
        (0x29c8,one-avg_a),(0x29d0,avg_a),(0x29d8,avg_ki),(0x29e4,f32!(65536.0)),
        (0x29e8,avg_kp),(0x2a04,F32::from(max_power)),
        (0x2b58,one-se_a),(0x2b5c,one-se_a1),(0x2b60,se_a),(0x2b64,se_a1),
        (0x2b68,f!("apple,se-ki")*dt),(0x2b6c,f!("apple,se-ki-1")*dt),
        (0x2b74,f32!(65536.0)),(0x2b78,f!("apple,se-kp")),(0x2b7c,f!("apple,se-kp-1")),
        (0x2b94,F32::from(release)),(0x2bc0,f32!(65536.0)),(0x2bc4,f32!(65536.0)),(0x2c30,f32!(65536.0))] { wf!(PP,o,v); }
    let mask = node.get_property::<u64>(c_str!("apple,fast-die0-sensor-mask"))?;
    w!(PP, 0x2268, mask as u32); w!(PP, 0x226c, (mask >> 32) as u32);
    w!(PP, 0x2f08, mask as u32); w!(PP, 0x2f0c, (mask >> 32) as u32);
    crate::g16_profile::calibration_words(board.calibration.as_ref().ok_or(EINVAL)?,
        board.sram_k, base as usize, |offset, value| words.push((Main, offset, value), GFP_KERNEL))?;
    w!(Globals,0xe4,0); w!(Globals,0xe8,0);
    dev_info!(dev.as_ref(),"G16G: J613 DT controllers sample_ms={} max_power_mw={} base={} ceiling={} firmware_thermal_controller={} fast_ki_dt_bits={:#x}\n",period,max_power,base,max,u8::from(thermal.is_some()),unsafe { core::mem::transmute::<F32,u32>(fast_ki) });
    Ok(words)
}
