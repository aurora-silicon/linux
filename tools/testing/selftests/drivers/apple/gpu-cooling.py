#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production percent-cap suite on the host, without a GPU."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
source = root / "drivers/gpu/drm/asahi/cooling_test.c"
with tempfile.TemporaryDirectory(prefix="asahi-cooling-") as work:
    binary = str(Path(work) / "cap-tests")
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-DASAHI_HOST_TEST", str(source), "-o", binary], check=True)
    subprocess.run([binary], check=True)
