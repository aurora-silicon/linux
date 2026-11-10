#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production Wi-Fi sleep hooks against asynchronous binding removal."""

from pathlib import Path
import resource
import subprocess
import sys
import tempfile

# python3 -I, as run-host-tests.py runs it, leaves this directory off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sleep_test_utils import function  # noqa: E402

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = (root / "drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c").read_text()
actual = "\n".join(
    function(source, name)
    for name in ("brcmf_pcie_sleep_finish", "brcmf_pcie_pm_enter_D3")
)
fixture = Path(__file__).with_suffix(".c").read_text()
variants = {
    "production": actual,
    "unlocked-cleanup": actual.replace("device_lock(dev);", "").replace(
        "device_unlock(dev);", ""
    ),
    "replacement-binding-cleanup": actual.replace(
        "if (!devinfo->sleep_reset_blocked)", "if (false)"
    ),
    "wrong-driver-cleanup": actual.replace(
        "if (dev->driver != &brcmf_pciedrvr.driver)", "if (false)"
    ),
    "missing-pending-cleanup": actual.replace(
        "devinfo->sleep_reset_blocked = true;", ""
    ),
}
with tempfile.TemporaryDirectory(prefix="brcmf-sleep-lifetime-") as tmp:
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    for name, code in variants.items():
        c.write_text(fixture.replace("/* ACTUAL_FUNCTIONS */", code))
        subprocess.run(
            [
                "cc",
                "-std=gnu11",
                "-pthread",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(c),
                "-o",
                str(exe),
            ],
            check=True,
        )
        result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
        if name == "production":
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert code != actual
            assert result.returncode != 0, f"mutation survived: {name}"
            print(f"PASS: rejected {name}")
