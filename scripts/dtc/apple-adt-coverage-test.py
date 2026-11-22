#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise coverage failures with synthetic DTS fixtures; needs dtc + pylibfdt."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).with_name("apple-adt-coverage.py")


class CoverageTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="adt-coverage-test-", dir=Path.cwd())
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.manifest = self.root / "t9999-j1.txt"
        source = self.root / "test.dts"
        source.write_text('''/dts-v1/;
            / { compatible = "apple,j1", "apple,t9999";
                soc { device@1000 { status = "disabled"; }; };
            };
        ''')
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o",
                        str(self.root / "t9999-j1.dtb"), str(source)], check=True)

    def run_check(self, text, *args):
        self.manifest.write_text(text)
        return subprocess.run([sys.executable, str(CHECKER), "--manifest-dir", str(self.root),
                               "--dtb-dir", str(self.root), *args], text=True, capture_output=True)

    def test_disabled_nodes_count_as_represented(self):
        result = self.run_check("/arm-io/device 0x1000 -> /soc/device@1000\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MISSING=0 mapped=1", result.stdout)

    def test_missing_is_fatal_and_report_preserves_count(self):
        text = "/arm-io/device 0x1000 -> MISSING\n"
        self.assertEqual(self.run_check(text).returncode, 1)
        result = self.run_check(text, "--report")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MISSING=1", result.stdout)

    def test_dangling_path_is_fatal_even_in_report(self):
        for mode in ((), ("--report",)):
            result = self.run_check("/arm-io/device 0x1000 -> /soc/device@2000\n", *mode)
            self.assertEqual(result.returncode, 1)
            self.assertIn("absent DT path", result.stderr)

    def test_reasoned_waiver(self):
        result = self.run_check("/arm-io/bookkeeping - -> WAIVED: firmware bookkeeping container\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("waived=1", result.stdout)

    def test_invalid_and_empty_manifests_fail_closed(self):
        for text in ("", "# comment only\n", "/x - -> WAIVED: \n", "/x - -> UNKNOWN\n",
                     "/x - /soc\n", "/x - -> /soc/../soc\n", "/x - -> /soc/\n",
                     "/x arbitrary-property-data -> /soc\n",
                     "/x - -> /soc //comment\n", "/x - -> /soc\n/x - -> /soc\n"):
            with self.subTest(text=text):
                self.assertEqual(self.run_check(text, "--report").returncode, 1)

    def test_missing_dtb_and_explicit_missing_manifest_fail(self):
        (self.root / "t9999-j1.dtb").unlink()
        self.assertEqual(self.run_check("/x - -> MISSING\n", "--report").returncode, 1)
        self.assertEqual(self.run_check("/x - -> MISSING\n", "--board", "t9999-j2").returncode, 1)

    def test_empty_selection_and_bad_board_fail(self):
        result = subprocess.run([sys.executable, str(CHECKER), "--manifest-dir", str(self.root),
                                 "--dtb-dir", str(self.root)], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.run_check("/x - -> /soc\n", "--board", "../t9999-j1").returncode, 1)

    def test_corrupt_dtb_fails(self):
        (self.root / "t9999-j1.dtb").write_bytes(b"not a DTB")
        self.assertEqual(self.run_check("/x - -> /soc\n").returncode, 1)

    def test_wrong_board_dtb_fails(self):
        wrong = self.root / "t9999-j2.dtb"
        wrong.write_bytes((self.root / "t9999-j1.dtb").read_bytes())
        (self.root / "t9999-j2.txt").write_text("/x - -> /soc\n")
        result = self.run_check("/x - -> /soc\n", "--board", "t9999-j2", "--report")
        self.assertEqual(result.returncode, 1)
        self.assertIn("root compatible does not match", result.stderr)

    def test_board_names_are_not_limited_to_j(self):
        dtb = (self.root / "t9999-j1.dtb").read_bytes()
        (self.root / "t9999-k1.dtb").write_bytes(dtb.replace(b"apple,j1\0", b"apple,k1\0"))
        (self.root / "t9999-k1.txt").write_text("/x - -> /soc\n")
        result = self.run_check("/x - -> /soc\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("t9999-k1: MISSING=0 mapped=1", result.stdout)

    def baseline(self, text):
        directory = self.root / "baseline"
        directory.mkdir(exist_ok=True)
        (directory / self.manifest.name).write_text(text)
        return ("--baseline-manifest-dir", str(directory), "--baseline-dtb-dir", str(self.root))

    def test_ratchet_accepts_backlog_and_reductions(self):
        args = self.baseline("/x - -> MISSING\n/y - -> MISSING\n")
        for text in ("/x - -> MISSING\n/y - -> MISSING\n",
                     "/x - -> /soc\n/y - -> MISSING\n"):
            result = self.run_check(text, *args)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_ratchet_rejects_more_missing_and_removed_entries(self):
        args = self.baseline("/x - -> /soc\n/y - -> MISSING\n")
        result = self.run_check("/x - -> MISSING\n/y - -> MISSING\n", *args)
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISSING increased", result.stderr)
        result = self.run_check("/x - -> /soc\n", *args)
        self.assertEqual(result.returncode, 1)
        self.assertIn("removed inventory devices", result.stderr)

    def test_ratchet_rejects_removed_manifest_and_new_broken_paths(self):
        args = self.baseline("/x - -> /soc\n")
        self.assertEqual(self.run_check("/x - -> /gone\n", *args).returncode, 1)
        self.manifest.unlink()
        result = subprocess.run([sys.executable, str(CHECKER), "--manifest-dir", str(self.root),
                                 "--dtb-dir", str(self.root), *args], capture_output=True)
        self.assertEqual(result.returncode, 1)

    def test_ratchet_compares_base_dtb_and_detects_node_removal(self):
        args = self.baseline("/x - -> /soc/device@1000\n")
        base_dtb = self.root / "old-dtb"
        base_dtb.mkdir()
        (base_dtb / "t9999-j1.dtb").write_bytes((self.root / "t9999-j1.dtb").read_bytes())
        # The path remains in both inventories, but vanishes from the head DTB.
        dts = self.root / "head.dts"
        dts.write_text('/dts-v1/; / { compatible="apple,j1", "apple,t9999"; soc {}; };')
        subprocess.run(["dtc", "-q", "-o", str(self.root / "t9999-j1.dtb"), str(dts)], check=True)
        result = self.run_check("/x - -> /soc/device@1000\n", *args[:-1], str(base_dtb))
        self.assertEqual(result.returncode, 1)
        self.assertIn("new absent DT path", result.stderr)

    def test_ratchet_existing_broken_path_is_backlog(self):
        args = self.baseline("/x - -> /old-gap\n")
        result = self.run_check("/x - -> /old-gap\n", *args)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ratchet_bootstrap_uses_head_inventory_and_base_dtb(self):
        directory = self.root / "baseline"
        directory.mkdir()
        result = self.run_check("/x - -> MISSING\n/y - -> /soc\n",
                                "--baseline-manifest-dir", str(directory),
                                "--baseline-dtb-dir", str(self.root))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("bootstrapping", result.stdout)

    def test_bootstrap_rejects_paths_absent_from_both_dtbs(self):
        directory = self.root / "baseline"
        directory.mkdir()
        result = self.run_check("/x - -> /never-existed\n",
                                "--baseline-manifest-dir", str(directory),
                                "--baseline-dtb-dir", str(self.root))
        self.assertEqual(result.returncode, 1)
        self.assertIn("new absent DT path", result.stderr)

    def test_bootstrap_of_a_new_board_needs_no_base_dtb(self):
        directory = self.root / "baseline"
        directory.mkdir()
        absent = self.root / "base-dtbs"
        absent.mkdir()
        args = ("--baseline-manifest-dir", str(directory), "--baseline-dtb-dir", str(absent))
        result = self.run_check("/x - -> MISSING\n/y - -> /soc\n", *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("new board, no base DTB", result.stdout)
        result = self.run_check("/x - -> /never-existed\n", *args)
        self.assertEqual(result.returncode, 1)
        self.assertIn("new absent DT path", result.stderr)

    def test_ratchet_validates_baseline_and_options(self):
        args = self.baseline("invalid\n")
        self.assertEqual(self.run_check("/x - -> /soc\n", *args).returncode, 1)
        self.assertEqual(self.run_check("/x - -> /soc\n", *args, "--report").returncode, 2)
        self.assertEqual(self.run_check("/x - -> /soc\n", *args[:2]).returncode, 2)
        self.assertEqual(self.run_check("/x - -> /soc\n", "--baseline-manifest-dir",
                                       str(self.root / "absent"), "--baseline-dtb-dir",
                                       str(self.root)).returncode, 2)


if __name__ == "__main__":
    unittest.main()
