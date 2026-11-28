#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the AMDGPU BO and Radeon VCE access paths with a mock BAR mapping."""

import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def extract(path, functions):
    lines = path.read_text().splitlines(keepends=True)
    parts = []
    for declaration in functions:
        start, = (i for i, line in enumerate(lines) if line.startswith(declaration))
        end = next(i for i in range(start + 1, len(lines)) if lines[i] == "}\n")
        parts.append("".join(lines[start:end + 1]))
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    gpu = ROOT / "drivers/gpu/drm"
    code = extract(gpu / "amd/amdgpu/amdgpu_object.h", [
        "static inline void amdgpu_bo_memset(",
        "static inline void amdgpu_bo_memcpy_to(",
        "static inline void amdgpu_bo_memcpy_from(",
    ])
    code += extract(gpu / "radeon/radeon_vce.c", ["int radeon_vce_resume("])
    (args.output / "bar-access.h").write_text(code)

    binary = args.output / "bar-safe"
    fixture = Path(__file__).with_name("bar-safe") / "fixture.c"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", args.output, fixture, "-o", binary], check=True)
    for scenario in ("amdgpu-ram", "amdgpu-posted", "amdgpu-nonposted",
                     "radeon-vce", "radeon-vce-legacy", "radeon-map-error"):
        subprocess.run([binary, scenario], check=True)
        print(f"{scenario}: pass", flush=True)


if __name__ == "__main__":
    main()
