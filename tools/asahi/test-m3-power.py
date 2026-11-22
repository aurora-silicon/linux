#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production controller writer and soft-float code without a GPU.

Kernel allocation/DT adapters are replaced by host adapters. Public production
power types and PStateTables are extracted as complete balanced Rust items,
never restated by hand. Their input hashes are printed with the test evidence.
"""
from pathlib import Path
import hashlib
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
M3 = ROOT / "drivers/gpu/drm/asahi/m3"


def item(source, anchor):
    start = source.index(anchor)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


shim = r'''
#![allow(dead_code)]
extern crate self as kernel;
pub type Error = i32;
pub type Result<T = ()> = core::result::Result<T, Error>;
pub const EINVAL: Error = 22;
pub const EOVERFLOW: Error = 75;
pub const ERANGE: Error = 34;
pub const GFP_KERNEL: () = ();
pub unsafe trait Zeroable {}
#[derive(Debug)]
pub struct KVec<T>(Vec<T>);
impl<T> Default for KVec<T> { fn default() -> Self { Self(Vec::new()) } }
impl<T> KVec<T> {
    pub fn new() -> Self { Self::default() }
    pub fn resize(&mut self, n: usize, value: T, _: ()) -> Result where T: Clone {
        self.0.resize(n, value); Ok(())
    }
}
impl<T> core::ops::Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> core::ops::DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }
pub mod prelude { pub use crate::{Error,Result,EINVAL,EOVERFLOW,ERANGE,GFP_KERNEL,KVec,Zeroable}; }
pub mod of {
    pub struct Property<'a>(pub &'a [u8]);
    pub trait PropertyUnit: Sized { const UNIT_SIZE: usize; fn from_bytes(data: &[u8]) -> crate::Result<Self>; }
    impl PropertyUnit for u32 {
        const UNIT_SIZE: usize = 4;
        fn from_bytes(data: &[u8]) -> crate::Result<Self> {
            Ok(u32::from_be_bytes(data.try_into().map_err(|_| crate::EINVAL)?))
        }
    }
    impl TryFrom<Property<'_>> for u32 {
        type Error = crate::Error;
        fn try_from(value: Property<'_>) -> crate::Result<Self> { Self::from_bytes(value.0) }
    }
}
'''
fixture = r'''
#[cfg(test)] mod controller_tests {
    use super::*;
    use float::F32;
    fn input() -> hw::PwrConfig {
        let mut p = hw::PwrConfig::default();
        p.perf_states = KVec((0..9).map(|i| hw::PState {
            freq_hz: if i == 8 { 1_338_000_000 } else { i * 100_000_000 },
            volt_mv: KVec(vec![if i == 0 { 125 } else { 450 + i * 50 }]), pwr_mw: i * 1000,
        }).collect());
        p.perf_base_pstate = 1;
        p.power_sample_period = 8;
        p.ppm_filter_time_constant_ms = 16;
        p.avg_power_filter_tc_ms = 16;
        p.pwr_filter_time_constant = 2;
        p.perf_filter_time_constant = 2;
        p.perf_filter_time_constant2 = 2;
        p.avg_power_target_filter_tc = 2;
        p.se_filter_time_constant = 2;
        p.se_filter_time_constant_1 = 2;
        p.pwr_min_duty_cycle = 30;
        p.avg_power_min_duty_cycle = 20;
        p
    }
    fn build(p: &hw::PwrConfig, sram: &[u32]) -> Result<m3_power_26::PowerImages> {
        m3_power_26::PowerImages::new(p, sram, 0xffff_fc21_0340_0000,
            0xffff_fc21_0350_0000, 0xffff_fc2f_fc00_0000, &[0; 53 * 40], None)
    }
    fn build_26b123(p: &hw::PwrConfig, sram: &[u32], records: &[u8])
        -> Result<m3_power_26::PowerImages> {
        m3_power_26::PowerImages::new(p, sram, 0xffff_fc21_0340_0000,
            0xffff_fc21_0350_0000, 0xffff_fc2f_fc00_0000, records,
            Some(m3_power_26::Abi26B123 { crash_copy_bytes: 0x8000, fast_die_target_c: 88 }))
    }
    #[test] fn the_26b123_images_move_the_26_6_2_words_and_take_the_machine_values() {
        let mut records = [0u8; 53 * 40];
        for (slot, record) in records.chunks_exact_mut(40).enumerate() {
            record[..8].copy_from_slice(&(0x1000 + slot as u64).to_le_bytes());
        }
        let p = input();
        let old = build(&p, &[950_001; 9]).unwrap();
        let new = build_26b123(&p, &[950_001; 9], &records).unwrap();
        assert_eq!((new.hardware.len(), new.performance.len(), new.power.len(), new.globals.len()),
            (0x2788, 0x6c48, 0x698, 0xe4c));
        // Register map: 0-7, 9-20, 22-24 keep their slots, 26-43 move up five, the rest stay empty.
        let at = |position: usize| 0x640 + position * 40;
        for (slot, position) in [(0, 0), (7, 7), (9, 9), (24, 24), (26, 31), (43, 48)] {
            assert_eq!(u64::from_le_bytes(new.hardware[at(position)..at(position) + 8].try_into().unwrap()),
                0x1000 + slot as u64, "slot {}", slot);
        }
        for position in [8, 21, 25, 26, 30, 49, 59] {
            assert!(new.hardware[at(position)..at(position) + 40].iter().all(|&b| b == 0), "position {}", position);
        }
        // Main: retention moves to the runtime pointers (+0xfa0 empty), +0xfa8 is the crash copy size.
        assert!(new.hardware[0xfa0..0xfa8] == [0; 8]);
        assert_eq!(u64::from_le_bytes(new.hardware[0xfa8..0xfb0].try_into().unwrap()), 0x8000);
        assert!(new.hardware[0xfb0..0xfdc] == old.hardware[0xe90..0xebc]);
        assert!(new.hardware[0xfdc..0x26d0] == old.hardware[0xec0..0x25b4]);
        // Words 26B123 adds or sets on top of the moved 26.6.2 ones (checked below).
        let masked = |b: &[u8], words: &[usize]| {
            let mut b = b.to_vec();
            for &at in words { b[at..at + 4].fill(0); }
            b
        };
        let perf = masked(&new.performance, &[0x2f34, 0x6ac8]);
        // Performance: the runs, the power-up words, the two added words and the shared3 state
        // word above every state.
        assert!(perf[..0x9c] == old.performance[..0x9c]);
        assert!(perf[0xac..0x28c0] == old.performance[0x9c..0x28b0]);
        // (26.6.2 source of each: +0x2f14 by the 0x2f08.. run, +0x6420 by the 0x63cc.. run)
        for (at, from) in [(0x2f34, 0x2f14), (0x6ac8, 0x6420)] {
            let was = word(&old.performance, from);
            assert_eq!(word(&new.performance, at), if was == 0 { 1 } else { was }, "perf +{:#x}", at);
        }
        for at in [0x9c, 0xa0, 0xa4, 0xa8] { assert_eq!(word(&new.performance, at), 1); }
        assert_eq!(word(&new.performance, 0x62b4), p.perf_states.len() as u32);
        // The rest of the block is only what the 26.6.2 build wrote there, moved by the run.
        assert!(new.performance[0x62ac..0x62b4] == old.performance[0x6264..0x626c]);
        assert!(new.performance[0x62b8..0x62fc] == old.performance[0x6270..0x62b4]);
        // Globals: one u32 inserted at 0x38, nothing else added.
        assert!(new.globals[..0x38] == old.globals[..0x38]);
        assert_eq!(word(&new.globals, 0x38), 0);
        assert!(new.globals[0x3c..0xe4c] == old.globals[0x38..0xe48]);
        // Power: the 26.6.2 words keep their positions, FastDie from the machine, nothing after.
        assert!(new.power[..0xc8] == old.power[..0xc8]);
        assert!(new.power[0xd0..0x150] == old.power[0xd0..0x150]);
        assert!(old.power[0x150..].iter().all(|&b| b == 0));
        assert_eq!(word(&new.power, 0xc8), 8800);
        assert_eq!(word(&new.power, 0xcc), 100 * p.fast_die0_release_temp);
        assert!(new.power[0x150..].iter().all(|&b| b == 0));
    }
    #[test] fn the_26b123_images_refuse_missing_or_inverted_machine_values() {
        let p = input();
        let records = [0u8; 53 * 40];
        for (bytes, target) in [(0, 88), (0x8001, 88), (0x20_0000, 88), (0x8000, 0), (0x8000, p.fast_die0_release_temp)] {
            assert!(m3_power_26::PowerImages::new(&p, &[950_001; 9], 0xffff_fc21_0340_0000,
                0xffff_fc21_0350_0000, 0xffff_fc2f_fc00_0000, &records,
                Some(m3_power_26::Abi26B123 { crash_copy_bytes: bytes, fast_die_target_c: target })).is_err(),
                "bytes {:#x} target {}", bytes, target);
        }
    }
    fn word(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
    #[test] fn native_controller_bounds_are_distinct_from_outputs_and_hardware_state() {
        let image = build(&input(), &[950_001; 9]).unwrap();
        assert_eq!(image.hardware.len(), 0x2658);
        assert_eq!(image.performance.len(), 0x6578);
        assert_eq!(image.power.len(), 0x670);
        assert_eq!(image.globals.len(), 0xe48);
        assert_eq!(image.maximum_khz, 1_338_000);
        assert_eq!(word(&image.performance, 0x2c), 1);
        assert_eq!(word(&image.performance, 0x30), 1);
        assert_eq!(word(&image.performance, 0x38), 0);
        assert_eq!(word(&image.performance, 0x9f0), 30);
        assert_eq!(word(&image.performance, 0xa38), 800);
        assert_eq!(word(&image.performance, 0xb78), 100);
        assert_eq!(word(&image.performance, 0xb7c), 800);
        assert_eq!(word(&image.hardware, 0x1408 + 64), 951);
        assert_eq!(word(&image.performance, 0x2268), 0x4248);
        assert_eq!(word(&image.performance, 0x3b58), 0x4b48);
    }
    #[test] fn malformed_tunables_and_nonfinite_gains_are_rejected() {
        for period in [0, 1001, u32::MAX] {
            let mut p = input(); p.power_sample_period = period;
            assert!(build(&p, &[950_000; 9]).is_err());
        }
        for bits in [0x7f800000, 0xff800000, 0x7fc00001] {
            let mut p = input(); p.pwr_integral_gain = F32::from_bits(bits);
            assert!(build(&p, &[950_000; 9]).is_err());
        }
        assert!(build(&input(), &[950_000; 8]).is_err());
        assert!(build(&input(), &[100_000; 9]).is_err());
        let mut p = input(); p.avg_power_target_filter_tc = u32::MAX;
        assert!(build(&p, &[950_000; 9]).is_err());
    }
    #[test] fn dt_order_is_not_the_firmware_voltage_order() {
        let p = input();
        let expected = build(&p, &[950_000; 9]).unwrap();
        let mut shuffled = input();
        shuffled.perf_states.swap(2, 6);
        let actual = build(&shuffled, &[950_000; 9]).unwrap();
        assert_eq!(actual.hardware.0, expected.hardware.0);
        assert_eq!(actual.performance.0, expected.performance.0);
    }
}
'''


def main():
    hw = (M3 / "hw.rs").read_text()
    init = (M3 / "initdata.rs").read_text()
    code = shim
    # Keep the production soft-float prefix verbatim. Its unrelated existing
    # print-only TODO test lacks an import and is not part of this harness.
    soft_float = (ROOT / "drivers/gpu/drm/asahi/float.rs").read_text()
    code += "mod float {\n" + soft_float.split("// TODO: Make this an actual test", 1)[0] + "\n}\n"
    code += "mod hw { use crate::{KVec,float::F32};\n"
    for name in ["PState", "PowerZone", "CsAfrPwrConfig", "PwrConfig"]:
        code += "#[derive(Default)]\n" + item(hw, "pub(crate) struct " + name + " {") + "\n"
    code += item(hw, "impl PState {") + "\n"
    code += "}\nmod initdata { use crate::{prelude::*,hw}; const PSTATE_TABLE_ENTRIES: usize = 16;\n"
    code += item(init, "pub(crate) struct PStateTables {") + "\n"
    code += item(init, "impl PStateTables {") + "\n}\n"
    mmio = (M3 / "m3_mmio_26.rs").read_text()
    code += ("mod m3_mmio_26 { pub const SLOTS:usize=53; pub const RECORD_BYTES:usize=40; pub const TABLE_OFFSET:usize=0x640;\n"
             + item(mmio, "pub(crate) const fn position_26b123") + "\n}\n")
    code += f'#[path = "{M3}/m3_power_26.rs"] mod m3_power_26;\n'
    code += fixture
    for path in [M3 / "hw.rs", M3 / "initdata.rs", M3 / "m3_power_26.rs", ROOT / "drivers/gpu/drm/asahi/float.rs"]:
        print(hashlib.sha256(path.read_bytes()).hexdigest(), path.relative_to(ROOT), flush=True)
    with tempfile.TemporaryDirectory(prefix="m3-power-") as directory:
        source = Path(directory) / "test.rs"
        source.write_text(code)
        for flags in [[], ["-O"]]:
            binary = Path(directory) / "tests"
            subprocess.run(["rustc", "--edition=2021", "--test", *flags, str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
