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


def closed_branches_put(text):
    """Every CLOSED admission is counted: each branch taking it must put it."""
    assert "gate < 0 || gate == APPLE_DART_WRITER_CLOSED" not in text
    found = 0
    for m in re.finditer(r"if \(gate == APPLE_DART_WRITER_CLOSED\)", text):
        exit_ = re.search(r"\b(return|continue)\b", text[m.end():])
        assert exit_, "unterminated CLOSED branch"
        body = text[m.end():m.end() + exit_.start()]
        if "apple_dart_sleep_writer_put(" not in body:
            line = text.count("\n", 0, m.start()) + 1
            raise AssertionError(f"apple-dart.c:{line}: CLOSED branch exits without a put")
        found += 1
    return found


driver = (root / "drivers/iommu/apple-dart.c").read_text()
branches = closed_branches_put(driver)
assert branches == 6, branches
# Negative control: a branch that leaks its reference is caught.
leak = driver.replace("\t\t\tapple_dart_sleep_writer_put(stream.dart, gate);\n\t\t\terr = err ?: ret;\n\t\t\tcontinue;",
                      "\t\t\terr = err ?: ret;\n\t\t\tcontinue;", 1)
assert leak != driver
try:
    closed_branches_put(leak)
except AssertionError:
    pass
else:
    raise SystemExit("negative control not caught")
print(f"DART callers: {branches} CLOSED branches each put their reference; 1 negative control")
