"""The installer's M3 MacBook Air (T8122) path, against the fake Mac of test_m3_flow.

An Air stays kernel-only unless its owner asks for m1n1's GPU handoff with
--m3-handoff, and then only when the release carries M3_AIR_M1N1_PACKAGE.
The handoff writes chosen.asahi,t8122-gpu=1 (and chosen.asahi,t8122-dcp=1
only with M3_AIR_DCP=1) to /etc/m1n1.conf. Every case sets
M3_AIR_M1N1_PACKAGE itself, so the tests don't depend on what the installer
ships with.
"""
from pathlib import Path
import re
import unittest

import test_m3_flow as flow

INSTALLER = flow.INSTALLER
SRC = INSTALLER.read_text()

AURORA8 = "m1n1-aurora-1.6.1.aurora8-1-aarch64.pkg.tar.zst"
AIR_GPU = b"chosen.asahi,t8122-gpu=1\n"
AIR_DCP = b"chosen.asahi,t8122-dcp=1\n"


def shell_value(name):
    return re.search(rf'^{name}="([^"]*)"$', SRC, re.M).group(1)


class M3AirTest(flow.M3FlowBase):
    def setUp(self):
        super().setUp()
        # aurora8: the T6030 handoff plus the T8122 GPU and display switches.
        self.fixture(AURORA8, None, ["asahi,t6030-gpu", "asahi,t6030-dcp", "asahi,t6030-dcpext",
                                     "asahi,t8122-gpu", "asahi,t8122-dcp"])
        self.air_pkg = f"{AURORA8} {self.shas[AURORA8]}"

    def sh(self, body, pkg=None, dcp=0, boards=None, check=True):
        pkg = self.air_pkg if pkg is None else pkg
        pre = f'M3_AIR_M1N1_PACKAGE="{pkg}"\nM3_AIR_DCP={dcp}\n'
        if boards is not None:
            pre += f'M3_HANDOFF_BOARDS="{boards}"\n'
        return self.run_sh(pre + body, check=check)

    def plan(self, try_=0, **kw):
        out = self.sh(f"M3_TRY={try_}\nm3_plan >/dev/null 2>&1\necho $M3_MODE", **kw).stdout
        return out.strip()

    def air_install(self, try_=0, check=True, **kw):
        return self.sh(f"M3_TRY={try_}\ninstall_all", check=check, **kw)

    # Which Macs are Airs

    def test_only_the_airs(self):
        flow.BOARDS["j433"] = ["apple,j433", "apple,t8122", "apple,arm-platform"]
        self.addCleanup(flow.BOARDS.pop, "j433")
        for board, want in [("j613", "yes"), ("j615", "yes"), ("j504", "no"), ("j433", "no"),
                            ("j516s", "no"), ("j514s", "no"), ("j314s", "no")]:
            with self.subTest(board=board):
                self.mac(board)
                self.assertEqual(self.sh("is_m3_air && echo yes || echo no").stdout.strip(), want)

    def test_listing_another_t8122_mac_gives_it_nothing(self):
        # Only M3 Pros and Airs have a handoff path; a listed J504 stays out.
        self.mac("j504")
        out = self.sh("is_m3_handoff_board && echo yes || echo no", boards="j516s j504").stdout
        self.assertEqual(out.strip(), "no")
        self.assertEqual(self.plan(boards="j516s j504"), "kernel")

    # Switches

    def test_switches(self):
        self.mac("j613")
        self.assertEqual(self.sh("m3_switches").stdout.strip(), "chosen.asahi,t8122-gpu=1")
        self.assertEqual(self.sh("m3_switches", dcp=1).stdout.strip(),
                         "chosen.asahi,t8122-gpu=1 chosen.asahi,t8122-dcp=1")
        # The M3 Pro keeps its three, whatever the Air's settings.
        self.mac("j516s")
        self.assertEqual(self.sh("m3_switches", dcp=1).stdout.strip(), shell_value("M3_SWITCHES"))

    def test_dcp_stays_off_in_the_installer(self):
        self.assertEqual(re.search(r"^M3_AIR_DCP=(\S+)$", SRC, re.M).group(1), "0")
        self.assertEqual(shell_value("M3_AIR_GPU_SWITCH"), "chosen.asahi,t8122-gpu=1")
        self.assertEqual(shell_value("M3_AIR_DCP_SWITCH"), "chosen.asahi,t8122-dcp=1")

    def test_no_air_on_by_default(self):
        # Every Air needs --m3-handoff until a tester's report adds its board.
        # Listing one is a release decision: change this test with it.
        listed = shell_value("M3_HANDOFF_BOARDS").split()
        airs = shell_value("M3_AIR_BOARDS").split()
        self.assertEqual(sorted(airs), ["j613", "j615"])
        self.assertFalse(set(listed) & set(airs), listed)

    def test_a_listed_air_needs_its_package(self):
        listed = set(shell_value("M3_HANDOFF_BOARDS").split())
        if listed & set(shell_value("M3_AIR_BOARDS").split()):
            self.mac("j613")
            self.assertEqual(self.run_sh("m3_air_package_ok && echo yes || echo no").stdout.strip(), "yes")

    def test_package_placeholder(self):
        self.mac("j613")
        good = "m1n1-aurora-1.6.1.aurora8-1-aarch64.pkg.tar.zst " + "a" * 64
        for pkg, want in [("", "no"), ("TODO", "no"), ("m1n1-aurora-1.6.1.aurora8-1-aarch64.pkg.tar.zst", "no"),
                          ("m1n1-aurora-1.6.1.aurora8-1-aarch64.pkg.tar.zst abc", "no"),
                          ("linux-aurora-x-aarch64.pkg.tar.zst " + "a" * 64, "no"),
                          (good + " extra", "no"), (good, "yes")]:
            with self.subTest(pkg=pkg):
                self.assertEqual(self.sh("m3_air_package_ok && echo yes || echo no", pkg=pkg).stdout.strip(), want)

    # Plan

    def test_plan(self):
        for board, stub, try_, pkg, boards, want in [
            ("j613", "14.8.3", 0, None, None, "kernel"),        # never by default
            ("j615", "14.8.3", 0, None, None, "kernel"),
            ("j613", "14.8.3", 0, "", None, "kernel"),          # no Air m1n1 in this release
            ("j613", "14.8.3", 1, None, None, "handoff"),       # the owner asked
            ("j615", "14.8.3", 1, None, None, "handoff"),
            ("j613", "14.8.3", 0, None, "j516s j613", "handoff"),   # listed later
            ("j613", "14.8.3", 0, "", "j516s j613", "kernel"),      # listed, but no m1n1
            ("j613", "15.6", 0, None, "j516s j613", "kernel"),      # listed, other stub
        ]:
            with self.subTest(board=board, stub=stub, try_=try_, pkg=pkg, boards=boards):
                self.mac(board, stub=stub)
                self.assertEqual(self.plan(try_=try_, pkg=pkg, boards=boards), want)

    def test_plan_messages(self):
        self.mac("j613")
        self.assertIn("case D", self.sh("m3_plan").stdout)
        self.assertIn("has no m1n1 with the", self.sh("m3_plan", pkg="").stdout)
        err = self.sh("M3_TRY=1\nm3_plan").stderr
        self.assertIn("in testing", err)
        self.assertIn("desktop keeps", err)

    def test_plan_try_refused(self):
        for board, stub, pkg, why in [("j613", "14.8.3", "", "no m1n1 with the M3 MacBook Air GPU"),
                                      ("j613", "15.6", None, "stub is 15.6"),
                                      ("j504", "14.8.3", None, "M3 MacBook Air (j613, j615)")]:
            with self.subTest(board=board, stub=stub, pkg=pkg):
                self.mac(board, stub=stub)
                proc = self.sh("M3_TRY=1\nm3_plan", pkg=pkg, check=False)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(why, proc.stderr)
                self.assertIn("Nothing was installed", proc.stderr)

    # /etc/m1n1.conf

    def test_conf_block(self):
        self.mac("j613")
        self.m1n1_conf.write_text("display=2560x1600\n")
        self.sh("m3_switches_write\nm3_switches_write")
        lines = self.m1n1_conf.read_text().splitlines()
        self.assertEqual(lines[0], "display=2560x1600")
        self.assertEqual([l for l in lines if l.startswith("chosen.")], ["chosen.asahi,t8122-gpu=1"])
        self.assertIn("# >>> aurora-sep: M3 Air handoff (remove with: install-aurora-sep.sh --uninstall)", lines)
        self.sh("m3_switches_remove")
        self.assertEqual(self.m1n1_conf.read_text(), "display=2560x1600\n")

    def test_conf_block_replaces_an_m3_pro_block(self):
        # A T6030 block in the file (say, copied from another Mac) never
        # survives next to the Air's.
        self.mac("j516s")
        self.sh("m3_switches_write")
        self.mac("j613")
        self.sh("m3_switches_write")
        chosen = [l for l in self.m1n1_conf.read_text().splitlines() if l.startswith("chosen.")]
        self.assertEqual(chosen, ["chosen.asahi,t8122-gpu=1"])

    def test_m3_pro_block_unchanged(self):
        self.mac("j516s")
        self.sh("m3_switches_write", dcp=1)
        text = self.m1n1_conf.read_text()
        self.assertTrue(text.startswith("# >>> aurora-sep: M3 Pro display and GPU handoff"), text)
        self.assertNotIn("t8122", text)

    # Package checks

    def test_package_check(self):
        pkgs = self.tmp / "pkgs"
        self.mac("j613")
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{pkgs / AURORA8}' && echo yes").stdout.strip(), "yes")
        # aurora6 has only the T6030 switches.
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{pkgs / flow.AURORA6}' && echo yes || echo no")
                         .stdout.strip(), "no")
        self.fixture("m1n1-gpu-only.pkg.tar.zst", None, ["asahi,t8122-gpu"])
        only = pkgs / "m1n1-gpu-only.pkg.tar.zst"
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{only}' && echo yes || echo no").stdout.strip(), "yes")
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{only}' && echo yes || echo no", dcp=1)
                         .stdout.strip(), "no")
        # The M3 Pro still needs its own three.
        self.mac("j516s")
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{only}' && echo yes || echo no").stdout.strip(), "no")

    def test_verify_bootbin(self):
        self.mac("j613")
        self.boot.write_bytes(b"M1N1:original\nDTBS:x\nUBOOT" + AIR_GPU)
        (self.fake / "m1n1.bin").write_text("M1N1:original\n")
        self.assertIn("M3 Air handoff switches", self.sh("m3_verify_bootbin").stdout)
        self.boot.write_bytes(b"M1N1:original\nDTBS:x\nUBOOT" + flow.SWITCHES)
        proc = self.sh("m3_verify_bootbin", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("M3 Air switch lines", proc.stderr)

    def test_packages(self):
        for board, try_, pkg, want in [("j613", 0, None, None), ("j613", 1, None, AURORA8),
                                       ("j613", 0, "", None), ("j516s", 0, None, flow.AURORA6)]:
            with self.subTest(board=board, try_=try_, pkg=pkg):
                self.mac(board)
                out = self.sh(f"M3_TRY={try_}\nm3_plan >/dev/null 2>&1\npackages_for_this_mac", pkg=pkg).stdout
                got = [line.split()[0] for line in out.splitlines()]
                self.assertEqual([p for p in got if p.startswith("m1n1-aurora-")], [want] if want else [])
                self.assertEqual(len(got), 5 if want else 4)

    # install_all and uninstall_all

    def test_plain_run_is_kernel_only(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        out = self.air_install().stdout
        self.assertIn("case D", out)
        self.assertIn("boot.bin is unchanged", out)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertFalse([d for d in self.downloaded() if d.startswith("m1n1-aurora-")])
        self.assertNotIn("update-m1n1 rebuilt", self.log())
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")
        self.assertFalse(self.m1n1_conf.exists())
        self.sh("uninstall_all")
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertFalse(self.update_conf.exists())

    def test_handoff_and_back(self):
        self.mac("j613")
        proc = self.air_install(try_=1)
        self.assertIn("still renders in software", proc.stdout)
        self.assertIn("M3 Air handoff switches", proc.stdout)
        self.assertIn("M3 MacBook Air (j613)", proc.stderr)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora8-1\n"), boot)
        self.assertTrue(boot.endswith(b"UBOOT" + AIR_GPU), boot)
        self.assertNotIn(b"t6030", boot)
        self.assertNotIn(b"t8122-dcp", boot)
        self.assertEqual(self.downloaded().count(AURORA8), 1)
        self.assertNotIn(flow.AURORA3, self.downloaded())
        self.assertNotIn(flow.AURORA6, self.downloaded())
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff")
        self.assertNotIn("M1N1_UPDATE_DISABLED", self.update_conf.read_text())

        self.sh("uninstall_all")
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-stock\n"), boot)
        self.assertNotIn(b"chosen.", boot)
        self.assertFalse(self.m1n1_conf.exists())
        self.assertFalse(self.state.exists())

    def test_rerun_keeps_the_handoff(self):
        self.mac("j615")
        self.air_install(try_=1)
        out = self.air_install().stdout
        self.assertIn("keeping it", out)
        self.assertTrue(self.boot.read_bytes().endswith(AIR_GPU))
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff")

    def test_rerun_without_an_air_package_stops(self):
        # A later build without the Air's m1n1 never drops a tested Air back
        # to kernel-only under a boot.bin it can't rebuild.
        self.mac("j613")
        self.air_install(try_=1)
        before = self.boot.read_bytes()
        proc = self.air_install(pkg="", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertIn("--uninstall", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)

    def test_display_switch(self):
        self.mac("j613")
        self.air_install(try_=1, dcp=1)
        self.assertTrue(self.boot.read_bytes().endswith(b"UBOOT" + AIR_GPU + AIR_DCP))

    def test_refused_without_an_air_package(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        proc = self.air_install(try_=1, pkg="", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -U", self.log())
        self.assertFalse(self.kept_copy().exists())

    def test_refused_with_an_m1n1_that_lacks_the_switch(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        proc = self.air_install(try_=1, pkg=f"{flow.AURORA6} {self.shas[flow.AURORA6]}", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("no M3 Air GPU handoff", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -U", self.log())

    def test_m3_pro_unchanged_by_air_settings(self):
        self.mac("j516s")
        out = self.air_install(dcp=1).stdout
        self.assertIn("built-in display at its native resolution", out)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora6-1\n"), boot)
        self.assertTrue(boot.endswith(flow.SWITCHES), boot)
        self.assertNotIn(AURORA8, self.downloaded())

    def test_m1_unchanged_by_air_settings(self):
        self.mac("j314s")
        self.air_install(dcp=1)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora3-1\n"), boot)
        self.assertNotIn(b"chosen.", boot)
        self.assertFalse(self.m1n1_conf.exists())


if __name__ == "__main__":
    unittest.main()
