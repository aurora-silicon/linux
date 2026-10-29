#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production PMP parsers without firmware or kernel mappings."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]


from common import block


def main():
    compiler = shutil.which("rustc")
    print("TAP version 13\n1..1", flush=True)
    if not compiler:
        print("ok 1 - PMP parsers # SKIP rustc unavailable")
        return 4
    source = (ROOT / "drivers/soc/apple/pmp.rs").read_text()
    program = r'''
type Result<T = ()> = core::result::Result<T, i32>;
const EINVAL: i32 = -22;
'''
    bootargs = ROOT / "drivers/soc/apple/pmp_bootargs.rs"
    program += "mod pmp_bootargs {" + bootargs.read_text().replace("//!", "//") + "}\n"
    for pattern in [r"^struct RegistryDescriptor\b",
                    r"^impl RegistryDescriptor\b"]:
        program += block(source, pattern) + "\n"
    program += r'''
fn main() {
    assert!(pmp_bootargs::region_valid(0x240, 12, 0x80000));
    assert!(!pmp_bootargs::region_valid(0x240, usize::MAX, 0x80000));
    let mut d = [0u8; 88];
    d[0] = b'x'; d[64..72].copy_from_slice(&16u64.to_le_bytes());
    d[72..80].copy_from_slice(&65535u64.to_le_bytes());
    let v = RegistryDescriptor::parse(&d).unwrap();
    assert_eq!(v.name[0], b'x'); assert_eq!(v.size, 16); assert_eq!(v.id, 65535);
    assert!(RegistryDescriptor::parse(&d[..87]).is_err());
    d[72..80].copy_from_slice(&65536u64.to_le_bytes());
    assert!(RegistryDescriptor::parse(&d).is_err());
    d[72..80].copy_from_slice(&0u64.to_le_bytes());
    d[64..72].copy_from_slice(&(u32::MAX as u64 + 1).to_le_bytes());
    assert!(RegistryDescriptor::parse(&d).is_err());
    d[64..72].copy_from_slice(&0u64.to_le_bytes()); d[..48].fill(b'a');
    assert!(RegistryDescriptor::parse(&d).is_err());
    let mut table = [0u8; 20];
    table[..4].copy_from_slice(&7u32.to_le_bytes());
    table[4..8].copy_from_slice(&4u32.to_le_bytes());
    table[12..16].copy_from_slice(&9u32.to_le_bytes());
    table[16..20].copy_from_slice(&16u32.to_le_bytes());
    let before = table;
    assert!(!pmp_bootargs::patch(&mut table, &[(7, 42)]));
    assert_eq!(table, before); // Later malformed entries must prevent all writes.
    assert!(pmp_bootargs::patch(&mut table[..12], &[(7, 42)]));
    assert_eq!(&table[8..12], &42u32.to_le_bytes());
    assert!(!pmp_bootargs::patch(&mut table[..7], &[(7, 42)]));
}
'''
    with tempfile.TemporaryDirectory(prefix="apif-pmp-test-") as directory:
        path = Path(directory) / "pmp"
        path.with_suffix(".rs").write_text(program)
        subprocess.run([compiler, "--edition=2021", "-Dwarnings",
                        str(path.with_suffix(".rs")), "-o", str(path)], check=True)
        subprocess.run([str(path)], check=True)
    print("ok 1 - PMP production descriptors and transactional boot arguments")
    return 0


if __name__ == "__main__":
    sys.exit(main())
