#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host fixtures for the actual AOP bootarg parser and hardware profiles."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]


def main():
    compiler = os.environ.get("RUSTC", "rustc")
    if shutil.which(compiler) is None:
        print("SKIP: rustc is required")
        return 4
    driver = ROOT / "drivers/soc/apple/aop.rs"
    helper = ROOT / "drivers/soc/apple/aop_bootargs.rs"
    source = driver.read_text()
    start = source.index("struct AopHwConfig {")
    end = source.index("kernel::of_device_table!", start)
    profiles = source[start:end]
    # Use the same FourCC conversion as the kernel provider.
    transport = (ROOT / "rust/kernel/soc/apple/aop.rs").read_text()
    abi_start = transport.index("pub enum ServiceABI {")
    abi_end = transport.index("\n}", abi_start) + 2
    abi = "#[derive(Clone, Copy, PartialEq, Eq)]\n" + transport[abi_start:abi_end]
    start = transport.index("pub const fn from_fourcc(")
    end = transport.index("\n}", start) + 2
    fourcc = transport[start:end]

    harness = """#![allow(dead_code)]
#[path = {helper}]
mod bootargs;

{fourcc}
{abi}
{profiles}

fn record(key: u32, data: &[u8]) -> Vec<u8> {{
    let mut bytes = key.to_le_bytes().to_vec();
    bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
    bytes.extend_from_slice(data);
    bytes
}}

#[test]
fn current_profiles_preserve_exact_legacy_values_and_unknown_fields() {{
    let cases = [
        (&HW_CFG_T8103, [0x020000u64, 0, 128, 1]),
        (&HW_CFG_T8112, [0x020000u64, 0, 128, 0]),
        (&HW_CFG_T6000, [0x020000u64, 0, 64, 0]),
        (&HW_CFG_T6020, [0x0100_00000000u64, 0, 64, 0]),
    ];
    let keys = [from_fourcc(b"EC0p"), from_fourcc(b"nCal"),
                from_fourcc(b"alig"), from_fourcc(b"AOPt")];
    for (profile, expected) in cases {{
        assert_eq!(profile.bootarg_overrides,
                   keys.iter().copied().zip(expected).collect::<Vec<_>>());
        let mut bytes = record(from_fourcc(b"keep"), &[0xbb; 12]);
        let prefix = bytes.clone();
        for (i, key) in keys.iter().enumerate() {{
            let size = if i == 0 {{ 8 }} else {{ 4 }};
            bytes.extend_from_slice(&record(*key, &vec![0xaa; size]));
        }}
        bootargs::patch(&mut bytes, profile.bootarg_overrides).unwrap();
        assert_eq!(&bytes[..prefix.len()], prefix);
        let mut offset = prefix.len();
        for (i, value) in expected.iter().enumerate() {{
            let size = if i == 0 {{ 8 }} else {{ 4 }};
            assert_eq!(&bytes[offset..offset + 4], &keys[i].to_le_bytes());
            assert_eq!(&bytes[offset + 4..offset + 8], &(size as u32).to_le_bytes());
            assert_eq!(&bytes[offset + 8..offset + 8 + size], &value.to_le_bytes()[..size]);
            offset += 8 + size;
        }}
    }}
}}

#[test]
fn j700_keeps_firmware_bootargs_untouched() {{
    assert!(HW_CFG_T8140.bootarg_overrides.is_empty());
    let mut bytes = vec![0xaa; 3];
    bootargs::patch(&mut bytes, HW_CFG_T8140.bootarg_overrides).unwrap();
    assert_eq!(bytes, vec![0xaa; 3]);
}}

#[test]
fn ec0p_only_override_preserves_all_other_firmware_fields() {{
    let mut bytes = record(from_fourcc(b"EC0p"), &[0xaa; 8]);
    for key in [from_fourcc(b"nCal"), from_fourcc(b"alig"), from_fourcc(b"AOPt")] {{
        bytes.extend_from_slice(&record(key, &[0xbb; 4]));
    }}
    let mut expected = bytes.clone();
    expected[8..16].copy_from_slice(&0x0100_00000000u64.to_le_bytes());
    bootargs::patch(&mut bytes, &[(from_fourcc(b"EC0p"), 0x0100_00000000)]).unwrap();
    assert_eq!(bytes, expected);
}}
""".format(helper=json.dumps(str(helper), ensure_ascii=False), profiles=profiles, fourcc=fourcc, abi=abi)

    with tempfile.TemporaryDirectory(prefix="apple-aop-bootargs-") as directory:
        directory = Path(directory)
        crate = directory / "fixtures.rs"
        binary = directory / "fixtures"
        crate.write_text(harness)
        subprocess.run([compiler, "--edition=2021", "--test", str(crate),
                        "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
