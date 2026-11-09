#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the agx_stats busy_ns integrator's KUnit cases on the host.

BusyIntegrator (drivers/gpu/drm/asahi/stats_busy.rs) turns the firmware's
Utilization windows into busy_ns. The cases pin that a window is weighted by
its utilization, that whole seconds are split from the tick remainder, and
that a PowerOff/PowerOn drops the baseline so the idle gap is never credited.

--negative-control turns power_changed() into a no-op and requires the cases
to fail, so the harness is shown to catch the over-count it guards against.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATS = ROOT / "drivers/gpu/drm/asahi/stats_busy.rs"
ATTR = "#[kernel::macros::kunit_tests(asahi_stats_busy)]"


def source(negative):
    text = STATS.read_text()
    start = text.index("pub(crate) struct BusyIntegrator")
    body = text[start:]
    assert ATTR in body, "KUnit suite attribute moved"
    body = body.replace(ATTR, "#[cfg(test)]")
    if negative:
        old = "        self.last_ts = None;\n"
        assert body.count(old) == 1
        body = body.replace(old, "")
    return "#![allow(dead_code)]\n" + body


def run(negative):
    with tempfile.TemporaryDirectory(prefix="asahi-busy-") as tmp:
        main = Path(tmp) / "main.rs"
        main.write_text(source(negative))
        binary = Path(tmp) / "test"
        subprocess.run(["rustc", "--edition=2021", "--test", "-Dwarnings", str(main), "-o", str(binary)],
                       check=True)
        return subprocess.run([str(binary), "--test-threads=1"],
                              capture_output=negative).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    rc = run(args.negative_control)
    if args.negative_control:
        if rc == 0:
            print("negative control: the cases passed without the baseline drop")
            return 1
        print("negative control: the cases fail without the baseline drop (expected)")
        return 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
