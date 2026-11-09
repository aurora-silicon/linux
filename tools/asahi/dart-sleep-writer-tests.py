#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import subprocess, tempfile, re

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/iommu/apple-dart-sleep.c").read_text()
actual = []
for name in (
    "apple_dart_sleep_writer_get",
    "apple_dart_sleep_writer_put",
    "apple_dart_sleep_close",
):
    m = re.search(
        r"^static (?:int|void) " + name + r"\([^;]+?\{.*?\n\}", source, re.M | re.S
    )
    assert m, name
    actual.append(m.group())
f = (
    Path(__file__)
    .with_suffix(".c")
    .read_text()
    .replace("/* ACTUAL_FUNCTIONS */", "\n".join(actual))
)
with tempfile.TemporaryDirectory() as t:
    p = Path(t) / "test.c"
    p.write_text(f)
    b = Path(t) / "test"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(p), "-o", str(b)],
        check=True,
    )
    subprocess.run([str(b)], check=True)
