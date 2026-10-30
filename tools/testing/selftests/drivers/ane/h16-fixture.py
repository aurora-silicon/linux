#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and verify SPEC-r2 H16G add-128 owner inputs without hardware."""

import argparse
import hashlib
import pathlib
import struct

HWX_SIZE = 49152
HWX_SHA256 = "231f54c28ea6b14a2867fac78eac1a2cb22aa3b0eeaa28113ed03c520b83a1d3"
EXPECTED_HASHES = {
    "a.bin": "3a944e8c891c9e4b00d1cd12bbec5f17cde432fafc1fce0df5809b3deb513e03",
    "b.bin": "779fff95d69316770cfff014c8bab4249c2f797c673a4518af80ab87b8d86a05",
    "expected.bin": "d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8",
}
FIRST_VALUES = (
    -2.25, -1.875, -1.5, -1.125, -0.75, -0.375, 0, 0.375,
    0.75, 1.125, 1.5, -0.875, -0.5, -0.125, 0.25, 0.625,
)


def buffers():
    a = [(i % 17 - 8) * 0.125 for i in range(16384)]
    b = [(i % 11 - 5) * 0.25 for i in range(16384)]
    expected = [x + y for x, y in zip(a, b)]
    if tuple(expected[:16]) != FIRST_VALUES:
        raise ValueError("unexpected CPU oracle")
    result = {
        "a.bin": b"".join(struct.pack("<e", x) for x in a),
        "b.bin": b"".join(struct.pack("<e", x) for x in b),
        "expected.bin": b"".join(struct.pack("<e", x) for x in expected),
    }
    for name, data in result.items():
        if len(data) != 32768 or hashlib.sha256(data).hexdigest() != EXPECTED_HASHES[name]:
            raise ValueError("fixture hash mismatch: " + name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hwx", type=pathlib.Path,
                        help="add-128 H16G HWX from pinned prepare_alu compiler")
    parser.add_argument("--out-dir", type=pathlib.Path,
                        help="write the verified inputs, oracle and poisoned output")
    args = parser.parse_args()
    data = buffers()
    if args.hwx:
        with args.hwx.open("rb") as source:
            hwx = source.read(HWX_SIZE + 1)
        if len(hwx) != HWX_SIZE or hashlib.sha256(hwx).hexdigest() != HWX_SHA256:
            parser.error("HWX is not the pinned SPEC-r2 H16G add-128 fixture")
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for name, value in data.items():
            (args.out_dir / name).write_bytes(value)
        (args.out_dir / "output.bin").write_bytes(b"\xff" * 32768)
    print("SPEC-r2 buffers: 3/3 hashes and 16384 fp16 oracle values verified")
    if args.hwx:
        print("SPEC-r2 HWX: size and SHA-256 verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
