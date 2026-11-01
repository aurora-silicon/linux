#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production motion report decoder and calibration selector on the host."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def const(source, name):
    start = source.index(f"const {name}")
    return source[start:source.index(";", start) + 1]


def main():
    root = Path(__file__).resolve().parents[5]
    motion = (root / "drivers/iio/common/aop_sensors/aop_motion.rs").read_text()
    parts = [const(motion, "REPORT_LEN"), const(motion, "XYZ_OFFSET"),
             block(motion, "fn decode("), block(motion, "fn le_u32("),
             block(motion, "fn location0_packet("),
             const(motion, "STATUS_UNSUPPORTED"),
             block(motion, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum CalStatus"), block(motion, "fn cal_status("), r'''
fn report(prefix: u8, xyz: [i32; 3], suffix: u8) -> Vec<u8> {
    let mut r = vec![prefix; 6];
    for v in xyz {
        r.extend_from_slice(&v.to_le_bytes());
    }
    r.extend_from_slice(&[suffix; 4]);
    r
}
#[test]
fn decodes_signed_axes() {
    // Synthetic reports; the bytes around the samples carry no sample.
    for (prefix, suffix) in [(0, 0), (0xff, 0xff), (0x5a, 0xa5)] {
        for xyz in [[65536, -65536, 0], [i32::MIN, i32::MAX, -1]] {
            assert_eq!(decode(&report(prefix, xyz, suffix)), Some(xyz));
        }
    }
}
#[test]
fn rejects_other_lengths() {
    let r = report(0, [1, 2, 3], 0);
    for len in 0..40 {
        if len == REPORT_LEN {
            continue;
        }
        let mut v = r.clone();
        v.resize(len, 0);
        assert_eq!(decode(&v), None);
    }
}
fn container(entries: &[(u32, &[u8])]) -> Vec<u8> {
    let mut c = vec![2, 0, entries.len() as u8, 1];
    for (location, packet) in entries {
        c.extend_from_slice(&location.to_le_bytes());
        c.extend_from_slice(&(packet.len() as u32).to_le_bytes());
        c.extend_from_slice(packet);
    }
    c
}
#[test]
fn calibration_status() {
    assert_eq!(cal_status(0), CalStatus::Taken);
    // Answered once the service holds a calibration from an earlier start.
    assert_eq!(cal_status(0xe000_02c7), CalStatus::Held);
    for status in [1, 0xe000_02bc, 0xe000_02c2, 0xe000_02e2, 0xe000_02c6, u32::MAX] {
        assert_eq!(cal_status(status), CalStatus::Refused, "{status:#x}");
    }
}
#[test]
fn selects_location_zero() {
    let a = [1u8; 44];
    let b = [2u8; 40];
    let one = container(&[(0, &a)]);
    assert_eq!(location0_packet(&one), Some(&a[..]));
    // Trailing bytes after the entries are not part of the packet.
    let mut padded = one.clone();
    padded.extend_from_slice(&[0; 4]);
    assert_eq!(location0_packet(&padded), Some(&a[..]));
    let two = container(&[(3, &b), (0, &a)]);
    assert_eq!(location0_packet(&two), Some(&a[..]));
    assert_eq!(location0_packet(&container(&[(1, &a)])), None);
}
#[test]
fn rejects_malformed_containers() {
    let a = [1u8; 44];
    let good = container(&[(0, &a)]);
    for len in 0..good.len() {
        assert_eq!(location0_packet(&good[..len]), None, "truncated to {len}");
    }
    let mut not_container = good.clone();
    not_container[3] = 0;
    assert_eq!(location0_packet(&not_container), None);
    let mut huge = good.clone();
    huge[8..12].copy_from_slice(&u32::MAX.to_le_bytes());
    assert_eq!(location0_packet(&huge), None);
    let mut many = good.clone();
    many[2] = 255;
    assert_eq!(location0_packet(&many), Some(&a[..]));
}
''']
    with tempfile.TemporaryDirectory(prefix="aop-motion-") as tmp:
        source = Path(tmp) / "test.rs"
        binary = Path(tmp) / "test"
        source.write_text("\n".join(parts))
        subprocess.run(shlex.split(os.environ.get("RUSTC", "rustc")) + [
            "--edition=2021", "--test", "-Dwarnings", str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
