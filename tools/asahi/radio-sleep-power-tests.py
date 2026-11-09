#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import re
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
src = (root / "drivers/pci/controller/pcie-apple-radio.c").read_text()


def extract(name):
    m = re.search(r"^static int " + name + r"\([^;]+?\{.*?\n\}", src, re.M | re.S)
    assert m, name
    return m.group()


actual = "\n".join(
    extract(n)
    for n in (
        "apple_radio_port_restore",
        "apple_radio_routes_restore",
        "apple_radio_recover",
    )
)
fixture = Path(__file__).with_suffix(".c").read_text()
spin = (root / "include/linux/soc/apple/sleep-spin.h").read_text()
spin = re.search(r"static inline int apple_sleep_spin.*?\n\}", spin, re.S).group()
fixture = fixture.replace("/* ACTUAL_SPIN */", spin)
with tempfile.TemporaryDirectory(prefix="radio-power-") as tmp:
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    c.write_text(fixture.replace("/* ACTUAL_FUNCTIONS */", actual))
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            str(c),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
