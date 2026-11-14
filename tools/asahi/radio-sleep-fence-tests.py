#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import subprocess, tempfile, re

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/pci/controller/pcie-apple-radio.c").read_text()


def function(name):
    m = re.search(r"^static int " + name + r"\([^;]+?\{.*?\n\}", source, re.M | re.S)
    assert m, name
    return m.group()


actual = "\n".join(
    function(n) for n in ("apple_radio_bme_hold", "apple_radio_bme_restore")
)
fixture = (
    Path(__file__)
    .with_suffix(".c")
    .read_text()
    .replace("/* ACTUAL_FUNCTIONS */", actual)
)
with tempfile.TemporaryDirectory() as t:
    p = Path(t) / "test.c"
    p.write_text(fixture)
    b = Path(t) / "test"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(p), "-o", str(b)],
        check=True,
    )
    subprocess.run([str(b)], check=True)
