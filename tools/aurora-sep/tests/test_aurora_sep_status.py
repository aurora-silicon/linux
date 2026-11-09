#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""aurora-sep status: one verdict per SEP state, from a synthetic sysfs.

The healthy values are the ones a fleet44 boot reports on the M1 Pro, M2,
J616s and MacBook Neo; each test then breaks one state and checks that the
tool names it and exits 1.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "aurora-sep"

HEALTHY = {
    "abi": "2", "profile": "T8112/J415", "boot": "warm", "protocol": "variant5",
    "xart": "enabled", "attach": "attached", "endpoints": "12",
    "endpoint_names": "cntl sbio scrd sse hdcp xars sks xarm hibe pnon skdl stac",
    "attach_ms": "845", "ready_ms": "4444", "keystore": "open",
    "keybag": "present", "sensor": "online", "touchid": "ready",
    "sensor_attach": "attached", "restore": "restored", "xart_guard": "1",
    "xart_guard_list": "loaded", "xart_guard_refusals": "0",
    "matching": "unverified", "os_identity": "explicit", "hwrng": "registered",
    "xarm_failures": "0", "unhandled_messages": "0", "sleep": "awake",
    "sleeps": "3",
}


class StatusTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.devices = self.root / "devices"
        self.sep = self.devices / "25e400000.sep"
        self.diag = self.sep / "diag"
        self.diag.mkdir(parents=True)
        drv = self.root / "drivers" / "apple_sep"
        drv.mkdir(parents=True)
        (self.sep / "driver").symlink_to(drv)
        for name, value in HEALTHY.items():
            self.set(name, value)
        (self.root / "proc" / "device-tree").mkdir(parents=True)
        self.compatible(b"apple,j413\0apple,t8112\0apple,arm-platform\0")
        (self.root / "proc" / "cmdline").write_text("root=UUID=x ro quiet\n")
        rng = self.root / "sys" / "class" / "misc" / "hw_random"
        rng.mkdir(parents=True)
        (rng / "rng_available").write_text("apple-sep\n")
        (rng / "rng_current").write_text("apple-sep\n")
        (self.root / "dev").mkdir()
        for node in ("sep-bio", "sep-key"):
            (self.root / "dev" / node).write_text("")
        (self.root / "dev" / "hwrng").write_bytes(bytes(range(1, 33)))

    def tearDown(self):
        self.tmp.cleanup()

    def set(self, name, value):
        path = self.diag / name
        if value is None:
            path.unlink(missing_ok=True)
        else:
            path.write_text(value + "\n")

    def compatible(self, raw):
        (self.root / "proc" / "device-tree" / "compatible").write_bytes(raw)

    def run_status(self, *args):
        env = dict(os.environ, AURORA_SEP_SYSFS=str(self.devices),
                   AURORA_SEP_ROOT=str(self.root), AURORA_SEP_NO_SYSTEM="1")
        out = subprocess.run([sys.executable, "-I", str(SCRIPT), "status", "--json", *args],
                             capture_output=True, text=True, env=env, timeout=30)
        report = json.loads(out.stdout)
        verdicts = {row["check"]: row["verdict"] for row in report["checks"]}
        return out.returncode, report, verdicts

    def test_healthy_boot_passes(self):
        rc, report, v = self.run_status()
        self.assertEqual(rc, 0, report)
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(report["totals"]["FAIL"], 0)
        self.assertEqual(v["endpoints"], "PASS")
        self.assertEqual(v["live"], "PASS")

    def test_a_missing_endpoint_fails(self):
        self.set("endpoint_names", "cntl sbio scrd sse hdcp xars xarm hibe pnon skdl stac")
        rc, report, v = self.run_status()
        self.assertEqual(rc, 1)
        self.assertEqual(v["endpoints"], "FAIL")
        row = [r for r in report["checks"] if r["check"] == "endpoints"][0]
        self.assertIn("missing sks", row["detail"])

    def test_an_old_kernel_falls_back_to_the_count(self):
        for name in ("endpoint_names", "attach_ms", "ready_ms", "os_identity",
                     "hwrng", "xarm_failures", "unhandled_messages", "sleep", "sleeps"):
            self.set(name, None)
        rc, _, v = self.run_status()
        self.assertEqual(rc, 0)
        self.assertEqual(v["endpoints"], "PASS")
        self.assertEqual(v["hwrng"], "PASS")
        self.set("endpoints", "7")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["endpoints"]), (1, "FAIL"))

    def test_a_closed_key_store_fails(self):
        self.set("keystore", "closed")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["keystore"]), (1, "FAIL"))

    def test_a_driver_failure_beyond_the_guard_fails(self):
        self.set("xart_guard_refusals", "2")
        self.set("xarm_failures", "2")
        rc, _, v = self.run_status()
        self.assertEqual(v["xarm-failures"], "PASS")
        self.assertEqual(v["xart-guard-refusals"], "WARN")
        self.set("xarm_failures", "3")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["xarm-failures"]), (1, "FAIL"))

    def test_no_os_identity_fails(self):
        self.set("os_identity", "unavailable")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["os-identity"]), (1, "FAIL"))

    def test_the_neo_needs_its_pin(self):
        self.compatible(b"apple,j700\0apple,t8140\0apple,arm-platform\0")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["os-uuid-pin"]), (1, "FAIL"))
        (self.root / "proc" / "cmdline").write_text(
            "ro apple_sep.os_uuid_hi=1 apple_sep.os_uuid_lo=2\n")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["os-uuid-pin"]), (0, "PASS"))

    def test_a_silent_enclave_fails_the_live_check(self):
        (self.root / "dev" / "hwrng").write_bytes(b"")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["live"]), (1, "FAIL"))
        rc, _, v = self.run_status("--no-live")
        self.assertEqual(rc, 0)
        self.assertNotIn("live", v)

    def test_unprovisioned_touch_id_warns(self):
        self.set("keybag", "missing")
        self.set("touchid", "failed")
        self.set("sensor", "unbound")
        self.set("sensor_attach", "unknown")
        self.set("restore", "unknown")
        rc, report, v = self.run_status()
        self.assertEqual(rc, 0, report)
        self.assertEqual((v["keybag"], v["touchid"]), ("WARN", "WARN"))

    def test_blocked_matching_fails(self):
        self.set("matching", "blocked")
        rc, _, v = self.run_status()
        self.assertEqual((rc, v["matching"]), (1, "FAIL"))

    def test_no_sep_device_exits_2(self):
        os.unlink(self.sep / "driver")
        for f in self.diag.iterdir():
            f.unlink()
        self.diag.rmdir()
        self.sep.rmdir()
        rc, report, _ = self.run_status()
        self.assertEqual(rc, 2)
        self.assertEqual(report["result"], "FAIL")

    def test_summary_is_one_line(self):
        env = dict(os.environ, AURORA_SEP_SYSFS=str(self.devices),
                   AURORA_SEP_ROOT=str(self.root), AURORA_SEP_NO_SYSTEM="1")
        out = subprocess.run([sys.executable, "-I", str(SCRIPT), "status", "--summary"],
                             capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(out.returncode, 0)
        self.assertEqual(len(out.stdout.splitlines()), 1)
        self.assertTrue(out.stdout.startswith("aurora-sep: PASS"))


if __name__ == "__main__":
    unittest.main()
