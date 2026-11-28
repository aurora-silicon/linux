#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test amdgpu VCN fwlog bounds validation and RLC CSB allocation failure paths."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def extract(path, declaration):
    lines = path.read_text().splitlines(keepends=True)
    start, = (i for i, line in enumerate(lines) if line.startswith(declaration))
    end = next(i for i in range(start + 1, len(lines)) if lines[i] == "}\n")
    return "".join(lines[start:end + 1])


def mutate(code, name):
    if name == "drop-vcn-header-bounds":
        old = (
            "\tif (header_size < sizeof(*plog) ||\n"
            "\t    header_size > AMDGPU_VCNFW_LOG_SIZE ||\n"
            "\t    read_pos < header_size || read_pos > AMDGPU_VCNFW_LOG_SIZE ||\n"
            "\t    write_pos < header_size || write_pos > AMDGPU_VCNFW_LOG_SIZE)\n"
            "\t\treturn -EFAULT;"
        )
        new = (
            "\tif (read_pos > AMDGPU_VCNFW_LOG_SIZE || "
            "write_pos > AMDGPU_VCNFW_LOG_SIZE)\n"
            "\t\treturn -EFAULT;"
        )
        assert old in code, "vcn bounds target missing"
        return code.replace(old, new, 1)
    if name == "drop-rlc-csb-enomem":
        old = (
            "\t\tif (!tmp) {\n"
            "\t\t\tdev_err(adev->dev,\n"
            "\t\t\t\t\"failed to allocate CSB staging buffer\\n\");\n"
            "\t\t\treturn -ENOMEM;\n"
            "\t\t}"
        )
        new = "\t\tif (!tmp)\n\t\t\treturn 0;"
        assert old in code, "rlc csb target missing"
        return code.replace(old, new, 1)
    raise ValueError(f"unknown mutation: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mutation", choices=("drop-vcn-header-bounds",
                                               "drop-rlc-csb-enomem"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    amdgpu = ROOT / "drivers/gpu/drm/amd/amdgpu"
    code = extract(amdgpu / "amdgpu_vcn.c",
                   "static ssize_t amdgpu_debugfs_vcn_fwlog_read(")
    code += "\n"
    code += extract(amdgpu / "amdgpu_rlc.c",
                    "int amdgpu_gfx_rlc_setup_csb(")
    if args.mutation:
        code = mutate(code, args.mutation)
    (args.out / "extracted.h").write_text(code)

    binary = args.out / "amdgpu-vcn-rlc"
    fixture = Path(__file__).with_name("amdgpu-vcn-rlc") / "fixture.c"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(args.out), str(fixture), "-o", str(binary)],
                   check=True)
    res = subprocess.run([str(binary)], check=False)
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
