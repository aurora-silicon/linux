#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise retained DART snapshots with both supported translation depths."""

from pathlib import Path
import sleep_test_utils as x
import re
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = (root / "drivers/iommu/apple-dart-sleep.c").read_text()
driver = (root / "drivers/iommu/apple-dart.c").read_text()
constants = (
    "DART_T8110_TLB_CMD",
    "DART_T8110_TLB_CMD_BUSY",
    "DART_T8110_PROTECT",
    "DART_T8110_PROTECT_LOCK",
    "DART_T8110_PROTECT_TTBR_TCR",
    "DART_T8110_ERROR_MASK",
    "DART_T8110_V2_ERROR_MASK",
    "DART_T8110_ENABLE_STREAMS",
    "DART_T8110_TCR",
    "DART_T8110_TCR_TRANSLATE_ENABLE",
    "DART_T8110_TCR_FOUR_LEVEL",
    "DART_T8110_TTBR",
    "DART_T8110_TTBR_VALID",
)
defines = "\n".join(
    re.search(r"^#define\s+" + name + r"\s+[^\n]+", driver, re.M).group()
    for name in constants
)
for name in ("DART_TCR", "DART_TTBR"):
    defines += (
        "\n"
        + re.search(
            r"^#define " + name + r"\([^\n]+(?:\n[^\n]+)*?(?=\n\n)", driver, re.M
        ).group()
    )
state = re.search(r"struct apple_dart_sleep_state \{.*?\n\};", source, re.S).group()
actual = "\n".join(
    x.function(source, name)
    for name in ("apple_dart_sleep_snapshot_error", "apple_dart_sleep_snapshot")
)
fixture, main = (
    Path(__file__).with_suffix(".c").read_text().split("/* ACTUAL_PRODUCTION */")
)
with tempfile.TemporaryDirectory(prefix="dart-snapshot-") as tmp:
    src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    for negative in (False, True):
        code = actual
        if negative:
            old = "value &= ~dart->hw->tcr_4level;"
            assert old in code
            code = code.replace(old, "(void)value;")
        src.write_text(fixture + defines + "\n" + state + "\n" + code + main)
        subprocess.run(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(src),
                "-o",
                str(binary),
            ],
            check=True,
        )
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if negative:
            assert result.returncode != 0, "Three-level-only admission was accepted"
            print("PASS: old admission rejected by captured four-level regression")
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
