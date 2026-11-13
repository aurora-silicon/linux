#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual native DART recovery under power loss and MMIO failures."""

from pathlib import Path
import re
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = (root / "drivers/iommu/apple-dart-sleep.c").read_text()


def extract(name):
    match = re.search(
        r"^(?:static )?(?:int|void) " + name + r"\([^;]+?\{.*?\n\}", source, re.M | re.S
    )
    assert match, name
    return match.group()


state = re.search(r"struct apple_dart_sleep_state \{.*?\n\};", source, re.S).group()
names = (
    "apple_dart_sleep_gapf",
    "apple_dart_sleep_power_capture",
    "apple_dart_sleep_power_ready",
    "apple_dart_sleep_tlb_idle",
    "apple_dart_sleep_idle",
    "apple_dart_sleep_power_off",
    "apple_dart_sleep_power_restore",
)
actual = "\n".join(extract(name) for name in names)
fixture = Path(__file__).with_suffix(".c").read_text()
spin = (root / "include/linux/soc/apple/sleep-spin.h").read_text()
spin = re.search(r"static inline int apple_sleep_spin.*?\n\}", spin, re.S).group()
fixture = fixture.replace("/* ACTUAL_SPIN */", spin)
code = fixture.replace("/* ACTUAL_STATE */", state).replace(
    "/* ACTUAL_FUNCTIONS */", actual
)
with tempfile.TemporaryDirectory(prefix="dart-power-") as tmp:
    src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    src.write_text(code)
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-I",
            str(root / "drivers/iommu"),
            str(src),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
