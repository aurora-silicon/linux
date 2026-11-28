#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the T6040/T6041 wire builders and unified calibration checks without a GPU."""

from pathlib import Path
import re
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[2] / "drivers/gpu/drm/asahi"


HEADER = r'''
#![allow(dead_code)]
extern crate self as kernel;
mod prelude {
    pub type KVec<T> = Vec<T>;
    pub use std::mem::size_of;
    pub type Result<T = ()> = std::result::Result<T, Error>;
    #[derive(Debug)]
    pub struct Error;
    impl From<std::num::TryFromIntError> for Error {
        fn from(_: std::num::TryFromIntError) -> Self { Self }
    }
    pub const EINVAL: Error = Error;
    pub const EIO: Error = Error;
    pub const EOVERFLOW: Error = Error;
    pub unsafe trait Zeroable {}
}
macro_rules! static_assert { ($e:expr) => { const _: () = assert!($e); } }
fn clear<T>(value: &mut T) {
    unsafe { std::ptr::write_bytes(value, 0, 1); }
}
#[macro_export]
macro_rules! c_str {
    ($s:literal) => { std::ffi::CStr::from_bytes_with_nul(concat!($s, "\0").as_bytes()).unwrap() };
}
pub mod str { pub use std::ffi::CStr; }
mod of {
    use crate::prelude::*;
    #[derive(Clone)]
    pub struct Node(pub std::collections::BTreeMap<String, Vec<u32>>);
    pub trait Property: Sized { fn decode(value: &[u32]) -> Result<Self>; }
    impl Property for u32 {
        fn decode(value: &[u32]) -> Result<Self> {
            if value.len() == 1 { Ok(value[0]) } else { Err(EINVAL) }
        }
    }
    /// Two big-endian cells, high word first, as rust/kernel/of.rs decodes an 8-byte u64.
    impl Property for u64 {
        fn decode(value: &[u32]) -> Result<Self> {
            if value.len() == 2 { Ok((u64::from(value[0]) << 32) | u64::from(value[1])) } else { Err(EINVAL) }
        }
    }
    impl Property for Vec<u32> { fn decode(value: &[u32]) -> Result<Self> { Ok(value.to_vec()) } }
    impl Node {
        pub fn get_property<T: Property>(&self, name: &crate::str::CStr) -> Result<T> {
            T::decode(self.0.get(name.to_str().unwrap()).ok_or(EINVAL)?)
        }
        pub fn get_opt_property<T: Property>(&self, name: &crate::str::CStr) -> Result<Option<T>> {
            self.0.get(name.to_str().unwrap()).map(|v| T::decode(v)).transpose()
        }
    }
}
mod driver {
    pub struct AsahiDrmDevice(pub DrmDevice);
    pub struct DrmDevice(pub Device);
    pub struct Device(pub crate::of::Node);
    impl AsRef<DrmDevice> for AsahiDrmDevice { fn as_ref(&self) -> &DrmDevice { &self.0 } }
    impl AsRef<Device> for DrmDevice { fn as_ref(&self) -> &Device { &self.0 } }
    impl Device { pub fn of_node(&self) -> Option<crate::of::Node> { Some(self.0.clone()) } }
}
mod float;
mod initdata;
mod g17 {
    pub mod fw { pub(crate) use crate::initdata; }
    mod calibration;
    pub fn check_property_adapter() {
        use crate::{driver::*, of::Node};
        let fields: &[(&str, &[u32])] = &[
            ("apple,t6040-handoff-version", &[2]),
            ("apple,core-mask", &[0xfffff]),
            ("apple,max-power-mw", &[70000]),
            ("apple,perf-base-pstate", &[1]),
            ("apple,perf-map-a", &[0,1,2,3,4,5,6,7,8,9,15,0,0,0,0,0]),
            ("apple,perf-map-b", &[0,1,2,3,4,5,6,7,8,9,15,0,0,0,0,0]),
            ("apple,perf-relative-a", &[0,10,20,30,40,50,60,70,80,90,100,0,0,0,0,0]),
            ("apple,perf-relative-b", &[0,0,11,22,33,44,55,66,77,88,100,0,0,0,0,0]),
            ("apple,core-leak-coef", &[0x44480000, 0x44800000]),
            ("apple,sram-leak-coef", &[0x43000000, 0x43000000]),
            ("apple,cs-leak-coef", &[0x43800000]),
            ("apple,afr-leak-coef", &[0x44000000]),
        ];
        let node = Node(fields.iter().map(|(k,v)| (k.to_string(), v.to_vec())).collect());
        let read = |n| calibration::Calibration::read(&AsahiDrmDevice(DrmDevice(Device(n))), 2);
        let chip = read(node.clone()).unwrap();
        assert_eq!(chip.max_power_mw, 70000);
        assert_eq!(chip.leakage[0], 0x44480000);
        assert_eq!(chip.leakage[1], 0x44800000);
        assert_eq!(chip.leakage[32..34], [0x43000000; 2]);
        assert_eq!(chip.leakage[52], 0x43800000);
        assert_eq!(chip.leakage[54], 0x44000000);
        assert!(chip.active_mcc_mask.is_none());
        assert!(chip.aon_table.is_none());
        for (key, _) in fields {
            let mut missing = node.clone(); missing.0.remove(*key);
            assert!(read(missing).is_err(), "accepted missing {key}");
            let mut short = node.clone(); short.0.get_mut(*key).unwrap().pop();
            assert!(read(short).is_err(), "accepted short {key}");
        }
        for key in ["apple,perf-map-a", "apple,perf-map-b", "apple,perf-relative-a", "apple,perf-relative-b"] {
            for index in 11..16 {
                let mut bad = node.clone(); bad.0.get_mut(key).unwrap()[index] = 1;
                assert!(read(bad).is_err(), "accepted nonzero tail {key}[{index}]");
            }
        }
        // Boundary power limits for two-cluster T6040.
        for ok_mw in [1, 200_000] {
            let mut ok = node.clone(); ok.0.insert("apple,max-power-mw".to_string(), vec![ok_mw]);
            assert_eq!(read(ok).unwrap().max_power_mw, ok_mw);
        }
        for (key, value) in [
            ("apple,t6040-handoff-version", 1),
            ("apple,core-mask", 0x100000),
            ("apple,max-power-mw", 0),
            ("apple,max-power-mw", 200_001),
            ("apple,perf-base-pstate", 0),
            ("apple,perf-base-pstate", 2),
            ("apple,cs-leak-coef", 0x00000000),
            ("apple,cs-leak-coef", 0x3f7fffff),
            ("apple,cs-leak-coef", 0x47800000),
            ("apple,cs-leak-coef", 0x7fc00000),
            ("apple,afr-leak-coef", 0xbf800000),
        ] {
            let mut bad = node.clone(); bad.0.insert(key.to_string(), vec![value]);
            assert!(read(bad).is_err(), "accepted invalid {key}={value:#x}");
        }
        // Unsupported cluster counts fail closed.
        for bad_clusters in [0, 1, 3, 8] {
            assert!(calibration::Calibration::read(
                &AsahiDrmDevice(DrmDevice(Device(node.clone()))),
                bad_clusters
            ).is_err());
        }
    }
    pub fn check_t6041_property_adapter() {
        use crate::{driver::*, initdata::RegisterWindow, of::Node};
        let key = |k: &[u8; 4]| u32::from_be_bytes(*k);
        let aon = [
            key(b"g0TR"), 1, 2, key(b"g1TR"), 3, 4, key(b"g2TR"), 5, 6,
            key(b"g3TR"), 7, 8, key(b"r0TR"), 9, 10,
        ];
        let fields: Vec<(&str, Vec<u32>)> = vec![
            ("apple,t6041-handoff-version", vec![2]),
            ("apple,core-mask", vec![0xf6, 0xfdbf_6fdb]),
            ("apple,active-mcc-bit-vector", vec![0x3f]),
            ("apple,max-power-mw", vec![118_500]),
            ("apple,perf-base-pstate", vec![1]),
            ("apple,perf-map-a", vec![0,1,2,3,4,6,8,10,12,14,15,0,0,0,0,0]),
            ("apple,perf-map-b", vec![0,1,2,3,4,5,7,9,11,13,15,0,0,0,0,0]),
            ("apple,perf-relative-a", vec![0,10,18,25,31,40,50,62,70,78,100,0,0,0,0,0]),
            ("apple,perf-relative-b", vec![0,0,7,14,21,35,50,64,78,92,100,0,0,0,0,0]),
            ("apple,core-leak-coef", vec![0x44f1_0000, 0x44f2_0000, 0x44f3_0000, 0x44f4_0000]),
            ("apple,sram-leak-coef", vec![0x4381_0000, 0x4382_0000, 0x4383_0000, 0x4384_0000]),
            ("apple,cs-leak-coef", vec![0x4390_0000]),
            ("apple,afr-leak-coef", vec![0x43c0_0000]),
            ("apple,aon-table", aon.to_vec()),
        ];
        let node = Node(fields.iter().map(|(k, v)| (k.to_string(), v.clone())).collect());
        let read_req = |n| calibration::Calibration::read(&AsahiDrmDevice(DrmDevice(Device(n))), 4);
        let read_opt = |n| calibration::Calibration::read_opt(&AsahiDrmDevice(DrmDevice(Device(n))), 4);

        // Mandatory path (32-core J616C) fails closed when handoff is absent; optional path
        // (40-core measured static capture) returns Ok(None).
        let mut absent = node.clone(); absent.0.remove("apple,t6041-handoff-version");
        assert!(read_req(absent.clone()).is_err());
        assert!(read_req(Node(Default::default())).is_err());
        assert!(read_opt(absent).unwrap().is_none());
        assert!(read_opt(Node(Default::default())).unwrap().is_none());

        // v1 handoff (e.g. aura#450 without leakage/max-power-mw) fails closed on mandatory
        // read (32-core J616C) but returns Ok(None) on optional read (40-core J614C/J575C
        // keeps booting on its measured static capture). Unsupported versions (0, 3) fail both.
        let mut v1 = Node(Default::default());
        v1.0.insert("apple,t6041-handoff-version".to_string(), vec![1]);
        assert!(read_req(v1.clone()).is_err());
        assert!(read_opt(v1).unwrap().is_none());
        for bad_ver in [0u32, 3] {
            let mut bv = node.clone();
            bv.0.insert("apple,t6041-handoff-version".to_string(), vec![bad_ver]);
            assert!(read_req(bv.clone()).is_err());
            assert!(read_opt(bv).is_err());
        }

        // Valid v2 handoff populates all four-cluster leakage slots, perf ladders, power ceiling,
        // MCC mask, and AON table.
        let chip = read_req(node.clone()).unwrap();
        assert_eq!(chip.perf.states_a, [0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15]);
        assert_eq!(chip.perf.states_b, [0, 1, 2, 3, 4, 5, 7, 9, 11, 13, 15]);
        assert_eq!(chip.perf.relative_a, [0, 10, 18, 25, 31, 40, 50, 62, 70, 78, 100]);
        assert_eq!(chip.perf.relative_b, [0, 0, 7, 14, 21, 35, 50, 64, 78, 92, 100]);
        assert_eq!(chip.perf.sram_min_mv, 810);
        assert_eq!(chip.max_power_mw, 118_500);
        assert_eq!(chip.leakage[0..4], [0x44f1_0000, 0x44f2_0000, 0x44f3_0000, 0x44f4_0000]);
        assert_eq!(chip.leakage[32..36], [0x4381_0000, 0x4382_0000, 0x4383_0000, 0x4384_0000]);
        assert_eq!(chip.leakage[52], 0x4390_0000);
        assert_eq!(chip.leakage[54], 0x43c0_0000);
        for (i, &w) in chip.leakage.iter().enumerate() {
            if !(0..4).contains(&i) && !(32..36).contains(&i) && i != 52 && i != 54 {
                assert_eq!(w, 0, "unexpected non-zero leakage slot {i}");
            }
        }
        assert_eq!(chip.active_mcc_mask, Some(0x3f));
        assert_eq!(chip.aon_table, Some(aon));
        assert!(chip.check_cores(&[0x3db, 0x3db, 0x3db, 0x3db]).is_ok());
        assert!(chip.check_cores(&[0x3ff, 0x3ff, 0x3ff, 0x3ff]).is_err());
        assert!(chip.check_cores(&[0x3db, 0x3db]).is_err());

        // apply_mcc sets slot-3 instances directly from active_mcc_mask.count_ones() and shifts
        // subsequent window VAs.
        let mut wins = [
            RegisterWindow {
                slot: 0, phys: 0x5_0241_4000, va: 0xffff_fc21_8020_0000,
                size: 0x4000, unk_18: 0, unk_20: 2, instances: 1, phys_stride: 0,
            },
            RegisterWindow {
                slot: 3, phys: 0x2_201c_4000, va: 0xffff_fc21_8020_8000,
                size: 0x1_8000, unk_18: 0, unk_20: 2, instances: 8, phys_stride: 0x200_0000,
            },
            RegisterWindow {
                slot: 4, phys: 0x5_0244_c000, va: 0xffff_fc21_802c_c000,
                size: 0x4000, unk_18: 0, unk_20: 0, instances: 1, phys_stride: 0,
            },
        ];
        chip.apply_mcc(&mut wins).unwrap();
        assert_eq!(wins[1].instances, 6);
        assert_eq!(wins[2].va, 0xffff_fc21_8029_c000);

        // Boundary power limits for four-cluster T6041.
        for ok_mw in [1, 250_000] {
            let mut ok = node.clone(); ok.0.insert("apple,max-power-mw".to_string(), vec![ok_mw]);
            assert_eq!(read_req(ok).unwrap().max_power_mw, ok_mw);
        }

        // Once version is present, every property is mandatory and full length.
        for (key, _) in &fields[1..] {
            let mut missing = node.clone(); missing.0.remove(*key);
            assert!(read_req(missing.clone()).is_err(), "accepted missing {key}");
            assert!(read_opt(missing).is_err(), "read_opt accepted missing {key}");
            let mut short = node.clone(); short.0.get_mut(*key).unwrap().pop();
            assert!(read_req(short.clone()).is_err(), "accepted short {key}");
            assert!(read_opt(short).is_err(), "read_opt accepted short {key}");
        }
        for key in ["apple,perf-map-a", "apple,perf-map-b", "apple,perf-relative-a", "apple,perf-relative-b"] {
            for index in 11..16 {
                let mut bad = node.clone(); bad.0.get_mut(key).unwrap()[index] = 1;
                assert!(read_req(bad).is_err(), "accepted nonzero tail {key}[{index}]");
            }
        }
        for (key, value) in [
            ("apple,t6041-handoff-version", vec![0]),
            ("apple,t6041-handoff-version", vec![1]),
            ("apple,t6041-handoff-version", vec![3]),
            ("apple,core-mask", vec![0, 0]),
            ("apple,core-mask", vec![0x100, 0]),
            ("apple,core-mask", vec![0xffff_ffff]),
            ("apple,active-mcc-bit-vector", vec![0]),
            ("apple,active-mcc-bit-vector", vec![0x100]),
            ("apple,max-power-mw", vec![0]),
            ("apple,max-power-mw", vec![250_001]),
            ("apple,perf-base-pstate", vec![0]),
            ("apple,perf-base-pstate", vec![2]),
            ("apple,perf-relative-a", vec![0,10,18,25,31,40,50,62,70,78,99,0,0,0,0,0]),
            ("apple,perf-relative-a", vec![0,10,18,25,24,40,50,62,70,78,100,0,0,0,0,0]),
            ("apple,perf-relative-b", vec![0,5,7,14,21,35,50,64,78,92,100,0,0,0,0,0]),
            ("apple,perf-relative-b", vec![0,0,7,14,21,35,50,64,78,92,99,0,0,0,0,0]),
            ("apple,perf-map-a", vec![0,1,2,3,4,6,8,10,12,14,14,0,0,0,0,0]),
            ("apple,core-leak-coef", vec![0x44f1_0000, 0x44f2_0000]),
            ("apple,sram-leak-coef", vec![0x4381_0000, 0x4382_0000, 0x4383_0000, 0x4384_0000, 0x4385_0000]),
        ] {
            let mut bad = node.clone(); bad.0.insert(key.to_string(), value);
            assert!(read_req(bad).is_err(), "accepted invalid {key}");
        }
        // NaN, zero, negative, <1.0, >=65536.0, +Inf leakage must be rejected on every slot.
        for bad_word in [0x0000_0000u32, 0x3f7f_ffff, 0x4780_0000, 0x7f80_0000, 0x7fc0_0000, 0xbf80_0000] {
            for idx in 0..4 {
                let mut bad_core = node.clone();
                bad_core.0.get_mut("apple,core-leak-coef").unwrap()[idx] = bad_word;
                assert!(read_req(bad_core).is_err(), "accepted bad core-leak[{idx}]={bad_word:#x}");
                let mut bad_sram = node.clone();
                bad_sram.0.get_mut("apple,sram-leak-coef").unwrap()[idx] = bad_word;
                assert!(read_req(bad_sram).is_err(), "accepted bad sram-leak[{idx}]={bad_word:#x}");
            }
            for key in ["apple,cs-leak-coef", "apple,afr-leak-coef"] {
                let mut bad = node.clone();
                bad.0.insert(key.to_string(), vec![bad_word]);
                assert!(read_req(bad).is_err(), "accepted bad {key}={bad_word:#x}");
            }
        }
        for row in 0..5 {
            let mut bad = node.clone(); bad.0.get_mut("apple,aon-table").unwrap()[row * 3] = key(b"g4TR");
            assert!(read_req(bad).is_err(), "accepted wrong AON key in row {row}");
        }
    }
}
'''

TEST = r'''
#[test]
fn property_adapter_requires_complete_chip_data() { g17::check_property_adapter(); }

#[test]
fn t6041_property_adapter_requires_v2_and_validates_all_fields() { g17::check_t6041_property_adapter(); }

#[test]
fn power_configuration_uses_only_its_inputs() {
    use initdata::v25g83::{Chip, PowerConfig25G83};
    let mut first = PowerConfig25G83([0xcc; 0x6510]);
    let mut leakage = [0; 56];
    leakage[0] = 0x44480000;
    leakage[1] = 0x44800000;
    leakage[2] = 0x44900000;
    leakage[3] = 0x44a00000;
    leakage[32] = 0x43000000;
    leakage[33] = 0x43100000;
    leakage[34] = 0x43200000;
    leakage[35] = 0x43300000;
    leakage[52] = 0x43800000;
    leakage[54] = 0x44000000;
    first.init(0x12340000, 0x56780000, 0xabcdef, 76543, &leakage);
    let word = |bytes: &[u8], off| u32::from_le_bytes(bytes[off..off + 4].try_into().unwrap());
    for off in [0xa04, 0xabc, 0x28c8, 0x28cc, 0x28d0, 0x2a08] {
        assert_eq!(word(&first.0, off), 76543);
    }
    for off in [0x9e0, 0x2a04] {
        assert_eq!(word(&first.0, off), (76543f32).to_bits());
    }
    assert_eq!(word(&first.0, 0x9ec), (-1000.0f32 / 76543f32).to_bits());
    for (i, value) in leakage.iter().enumerate() {
        assert_eq!(word(&first.0, 0x642c + 4 * i), *value);
    }
    assert_eq!(u64::from_le_bytes(first.0[0x3b50..0x3b58].try_into().unwrap()), 0x12340080);
    assert_eq!(u64::from_le_bytes(first.0[0x6318..0x6320].try_into().unwrap()), 0x56780000);
    assert_eq!(word(&first.0, 0x63d4), 0xabcdef);
    let mut second = PowerConfig25G83([0; 0x6510]);
    second.init(0x12340000, 0x56780000, 0xabcdef, 76543, &leakage);
    assert_eq!(first.0, second.0);

    // Dynamic 0x9ec matches known reference values bit-for-bit (0 ULP).
    for (mw, expected_9ec) in [
        (69_116u32, 0xbc6d_0cfeu32),
        (110_586, 0xbc14_27fc),
        (129_702, 0xbbfc_a403),
    ] {
        second.init(0x12340000, 0x56780000, 0xabcdef, mw, &[0; 56]);
        assert_eq!(word(&second.0, 0xa04), mw);
        assert_eq!(word(&second.0, 0x9e0), (mw as f32).to_bits());
        assert_eq!(word(&second.0, 0x9ec), expected_9ec, "0x9ec mismatch for {mw} mW");
        for i in 0..56 {
            assert_eq!(word(&second.0, 0x642c + 4 * i), 0);
        }
    }

    // apply_chip refuses static 0x9ec overrides and preserves dynamic 0x9ec otherwise.
    let bad_chip = Chip {
        max_power_mw: None,
        leakage: None,
        aon_table: true,
        hw_data_words: &[],
        power_config_words: &[(0x9ec, 0xbc14_27fc, 1)],
        perf_a_replicate_lanes: false,
        cs_perf_table: true,
    };
    assert!(second.apply_chip(&bad_chip).is_err());

    let good_chip = Chip {
        max_power_mw: None,
        leakage: None,
        aon_table: true,
        hw_data_words: &[],
        power_config_words: &[(0x3b5c, 0x2a22_2aa2, 1), (0x6360, 0x5, 1)],
        perf_a_replicate_lanes: false,
        cs_perf_table: true,
    };
    second.apply_chip(&good_chip).unwrap();
    assert_eq!(word(&second.0, 0x9ec), 0xbbfc_a403);
    assert_eq!(word(&second.0, 0x3b5c), 0x2a22_2aa2);
    assert_eq!(word(&second.0, 0x6360), 0x5);

    // 26B123 PowerState receives the resolved max_power_mw at +0x12c.
    let mut ps = initdata::v26b123::PowerState([0; 0x6d8]);
    ps.init(118_500);
    assert_eq!(word(&ps.0, 0x12c), 118_500);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="asahi-t6040-") as tmp:
        root = Path(tmp)
        (root / "initdata").mkdir()
        (root / "g17").mkdir()
        for source, target in [
            ("g17/fw/initdata.rs", "initdata.rs"),
            ("g17/fw/initdata/v25g83.rs", "initdata/v25g83.rs"),
            ("g17/fw/initdata/v26b123.rs", "initdata/v26b123.rs"),
        ]:
            text = (DRIVER / source).read_text()
            (root / target).write_text(text.replace("use kernel::prelude::*;", "use crate::prelude::*;"))
        text = (DRIVER / "float.rs").read_text().split("impl<'a> TryFrom<of::Property")[0]
        text = re.sub(r"use kernel::\{.*?\};", "use crate::prelude::*;", text, flags=re.S)
        (root / "float.rs").write_text(text)
        text = (DRIVER / "g17/calibration.rs").read_text()
        text = text.replace("use kernel::{c_str,", "use kernel::{")
        text = text.replace("#[kunit_tests(asahi_t6040_calibration)]", "#[cfg(test)]")
        (root / "g17/calibration.rs").write_text(text)
        (root / "main.rs").write_text(HEADER + TEST)
        binary = root / "test"
        subprocess.run(["rustc", "--edition=2021", "--test", "-Dwarnings",
                        str(root / "main.rs"), "-o", str(binary)], check=True)
        subprocess.run([str(binary), "--test-threads=1"], check=True)


if __name__ == "__main__":
    main()
