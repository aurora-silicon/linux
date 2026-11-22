#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production retained-link snapshot, including an operating link with BUSY set."""

from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile

# python3 -I, as run-host-tests.py runs it, leaves this directory off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sleep_test_utils as x  # noqa: E402

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
radio = (root / "drivers/pci/controller/pcie-apple-radio.c").read_text()
pcie = (root / "drivers/pci/controller/pcie-apple.c").read_text()
constants = (
    "PORT_INTMSK",
    "PORT_APPCLK",
    "PORT_STATUS",
    "PORT_STATUS_READY",
    "PORT_LINKSTS",
    "PORT_LINKSTS_UP",
    "PORT_LINKSTS_BUSY",
    "PORT_MSICFG",
    "PORT_MSICFG_EN",
    "PORT_MSIBASE",
    "PORT_RID2SID_VALID",
    "PORT_RID2SID_SID_SHIFT",
)
defines = "\n".join(
    re.search(r"^#define\s+" + name + r"\s+[^\n]+", pcie, re.M).group()
    for name in constants
)
actual = "\n".join(
    (
        x.function(pcie, "apple_pcie_managed_word"),
        x.function(radio, "apple_radio_snapshot_error"),
        x.function(radio, "apple_radio_snapshot"),
    )
)
fixture, main = (
    Path(__file__).with_suffix(".c").read_text().split("/* ACTUAL_PRODUCTION */")
)
with tempfile.TemporaryDirectory(prefix="radio-snapshot-") as tmp:
    src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    for negative in (False, True):
        code = actual
        if negative:
            old = "if (!(value & PORT_LINKSTS_UP))"
            assert old in code
            code = code.replace(
                old,
                "if ((value & (PORT_LINKSTS_UP | "
                "PORT_LINKSTS_BUSY)) != PORT_LINKSTS_UP)",
            )
        src.write_text(fixture + defines + "\n" + code + main)
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
            assert result.returncode != 0, "Old stopped-port check was accepted"
            print("PASS: old link-state check rejected by captured-status regression")
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
