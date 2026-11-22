#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the native host's real noirq lifecycle, including a refused sleep."""

from pathlib import Path
import re, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
s = (root / "drivers/pci/controller/pcie-apple-radio.c").read_text()
names = (
    "apple_radio_config_allowed",
    "apple_radio_bme_hold",
    "apple_radio_bme_restore",
    "apple_radio_valid",
    "apple_radio_commands_held",
    "apple_radio_seal",
    "apple_radio_abort",
    "apple_radio_finish",
    "apple_radio_suspend_noirq",
    "apple_radio_resume_noirq",
)
functions = []
for n in names:
    m = re.search(
        r"^static (?:int|void|bool) " + n + r"\([^;]+?\{.*?\n\}", s, re.M | re.S
    )
    assert m, n
    functions.append(m.group())
code = (
    Path(__file__)
    .with_suffix(".c")
    .read_text()
    .replace("/* ACTUAL_FUNCTIONS */", "\n".join(functions))
)
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    p.write_text(code)
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(p), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
