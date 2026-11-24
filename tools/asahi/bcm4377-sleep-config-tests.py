#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise private PCI-window restoration before Bluetooth resume IRQs."""

from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/bluetooth/hci_bcm4377.c").read_text()
functions = []
for name in ("bcm4377_init_cfg", "bcm4377_resume_noirq"):
    match = re.search(
        r"^static int " + name + r"\([^;]+?\{.*?\n\}", source, re.M | re.S
    )
    assert match, name
    functions.append(match.group())
constants = "\n".join(
    line for line in source.splitlines() if line.startswith("#define BCM4377_PCIECFG_")
)
code = (
    Path(__file__)
    .with_suffix(".c")
    .read_text()
    .replace("/* ACTUAL */", constants + "\n" + "\n".join(functions))
)
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "test.c"
    c.write_text(code)
    exe = Path(tmp) / "test"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
