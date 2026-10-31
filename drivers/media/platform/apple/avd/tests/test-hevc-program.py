#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the actual instruction builders and compare every word, on a host.

--baseline-ref must identify the unconverted driver. The shim supplies DMA
addresses and V4L2 controls; it never emulates the device or claims pixel proof.
"""
import argparse
import itertools
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REPO_PATH = "drivers/media/platform/apple/avd/avd-hevc.c"


def body(source):
    return source[source.index("#define NEW_TILE_ID"):source.index("static void update_dec_buf_info")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    args = parser.parse_args()
    old = subprocess.check_output(["git", "show", f"{args.baseline_ref}:{REPO_PATH}"], text=True)
    new = (ROOT / "avd-hevc.c").read_text()
    cc = os.environ.get("HOSTCC", "cc")
    common = [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
              "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-unused-function",
              "-DAVD_HEVC_HOST_TEST", "-I", str(ROOT), "-I", str(ROOT / "tests")]
    with tempfile.TemporaryDirectory(prefix="avd-hevc-program-") as temp:
        temp = Path(temp)
        binaries = []
        for name, source in (("legacy", old), ("frame", new)):
            path = temp / f"{name}.c"
            path.write_text('#include "hevc-shim.h"\n' + body(source) +
                            (ROOT / "tests/hevc-program-main.c").read_text())
            binary = temp / name
            command = common + (["-DHEVC_LEGACY"] if name == "legacy" else [])
            command += [str(path), str(ROOT / "avd-hevc-fp.c"),
                        str(ROOT / "avd-hevc-slices.c"), "-o", str(binary)]
            subprocess.run(command, check=True)
            binaries.append(binary)
        count = 0
        for values in itertools.product((8, 10), (32, 64), (0, 1), (0, 1, 2), range(4)):
            values = list(map(str, values))
            expected = subprocess.check_output([str(binaries[0]), *values, "0"])
            for layout in ("0", "1"):
                actual = subprocess.check_output([str(binaries[1]), *values, layout])
                if actual != expected:
                    raise AssertionError(f"Instruction mismatch: {values}, layout {layout}")
                count += 1
        print(f"PASS: {count} Main/Main10 CTU32/64 I/P/B tile and slice-layout instruction comparisons")


if __name__ == "__main__":
    main()
