#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise apple-sibling-drift.py with synthetic DTS fixtures; needs dtc + pylibfdt."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).with_name("apple-sibling-drift.py")


def dts(board, *props):
    return '''/dts-v1/;
        / { compatible = "apple,%s", "apple,t8112", "apple,arm-platform";
            #address-cells = <1>; #size-cells = <0>;
            dcp@1000 { reg = <0x1000>; %s };
        };
    ''' % (board, " ".join(props))


class SiblingDriftTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="drift-test-", dir=Path.cwd())
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.base = self.root / "base"
        self.head = self.root / "head"
        self.base.mkdir()
        self.head.mkdir()
        # The J473's HDMI GPIO is right drift: the baseline has it.
        for directory in (self.base, self.head):
            self.dtb(directory, "t8112-j413", dts("j413", "shared;"))
            self.dtb(directory, "t8112-j473", dts("j473", "shared;", "hdmi-gpios = <1>;"))

    def dtb(self, directory, name, source):
        path = self.root / (name + ".dts")
        path.write_text(source)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o",
                        str(directory / (name + ".dtb")), str(path)], check=True)

    def check(self, *args):
        return subprocess.run([sys.executable, str(CHECKER), "--dtb-dir", str(self.head),
                               "--baseline-dtb-dir", str(self.base), *args],
                              text=True, capture_output=True)

    def test_unchanged_is_quiet(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("drift 1, new 0, resolved 0", result.stderr)

    def test_one_board_fix_warns(self):
        self.dtb(self.head, "t8112-j473",
                 dts("j473", "shared;", "hdmi-gpios = <1>;", "apple,fix;"))
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("t8112 /dcp@1000 apple,fix: only on j473; j413 have the node without it",
                      result.stdout)
        self.assertNotIn("hdmi-gpios", result.stdout)
        self.assertEqual(self.check("--strict").returncode, 1)

    def test_fix_on_every_board_is_quiet(self):
        self.dtb(self.head, "t8112-j413", dts("j413", "shared;", "apple,fix;"))
        self.dtb(self.head, "t8112-j473",
                 dts("j473", "shared;", "hdmi-gpios = <1>;", "apple,fix;"))
        result = self.check("--strict")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_dropping_a_shared_property_from_one_board_warns(self):
        self.dtb(self.head, "t8112-j413", dts("j413"))
        result = self.check()
        self.assertIn("t8112 /dcp@1000 shared: only on j473; j413", result.stdout)

    def test_resolved_drift_is_counted_and_variants_are_ignored(self):
        self.dtb(self.head, "t8112-j413", dts("j413", "shared;", "hdmi-gpios = <1>;"))
        self.dtb(self.head, "t8112-j413-fw27", dts("j413", "apple,variant-only;"))
        result = self.check("--strict")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("drift 0, new 0, resolved 1", result.stderr)


if __name__ == "__main__":
    unittest.main()
