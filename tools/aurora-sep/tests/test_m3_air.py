"""The installer's M3 MacBook Air (T8122) path, against the fake Mac of test_m3_flow.

An Air stays kernel-only unless its owner asks for m1n1's GPU handoff with
--m3-handoff. It then gets the one m1n1 every Mac gets (M1N1_PACKAGE), and
the handoff writes chosen.asahi,t8122-gpu=1 (and chosen.asahi,t8122-dcp=1
only with M3_AIR_DCP=1) to /etc/m1n1.conf. Every case sets the m1n1 package
and M3_AIR_DRY_RUN itself, so the tests don't depend on what the installer
ships with. M3AirDryRunTest covers the dry run, which
writes M3_AIR_DRY_RUN_SWITCHES instead, and the display handoff
(M3_AIR_DISPLAY_HANDOFF=1), which writes the same switches under its own
variant name.
"""
from pathlib import Path
import re
import shutil
import unittest

import test_m3_flow as flow

INSTALLER = flow.INSTALLER
SRC = INSTALLER.read_text()

AURORA8 = "m1n1-aurora-1.6.1.aurora8-1-aarch64.pkg.tar.zst"
AIR_GPU = b"chosen.asahi,t8122-gpu=1\n"
AIR_DCP = b"chosen.asahi,t8122-dcp=1\n"


def shell_value(name):
    return re.search(rf'^{name}="([^"]*)"$', SRC, re.M).group(1)


def shell_word(name):
    return re.search(rf'^{name}=(\S+)$', SRC, re.M).group(1)


DISPLAY_VARIANT = shell_value("M3_AIR_DISPLAY_VARIANT")


class M3AirTest(flow.M3FlowBase):
    def setUp(self):
        super().setUp()
        # aurora8: the T6030 handoff plus the T8122 GPU and display switches.
        self.fixture(AURORA8, None, ["asahi,t6030-gpu", "asahi,t6030-dcp", "asahi,t6030-dcpext",
                                     "asahi,t8122-gpu", "asahi,t8122-dcp"])
        # The one m1n1 every Mac of this fake release gets.
        self.m1n1_pkg = AURORA8

    dry_run = 0

    def sh(self, body, pkg=None, dcp=0, boards=None, check=True, display=0, variant=None):
        # pkg: another fixture as this release's one m1n1 package.
        pre = (f'M3_AIR_DCP={dcp}\nM3_AIR_DRY_RUN={self.dry_run}\n'
               f'M3_AIR_DISPLAY_HANDOFF={display}\n')
        if pkg is not None:
            pre += f'M1N1_PACKAGE="{pkg} {self.shas[pkg]}"\n'
            pre += f'M1N1_BIN_SHA={self.bin_shas[pkg]}\n'
        if variant is not None:
            pre += f'M3_AIR_DISPLAY_VARIANT={variant}\n'
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



    # Plan

    def test_plan(self):
        for board, stub, try_, pkg, boards, want in [
            ("j613", "14.8.3", 0, None, None, "kernel"),        # never by default
            ("j615", "14.8.3", 0, None, None, "kernel"),
            ("j613", "14.8.3", 1, None, None, "handoff"),       # the owner asked
            ("j615", "14.8.3", 1, None, None, "handoff"),
            ("j613", "14.8.3", 0, None, "j516s j613", "handoff"),   # listed later
            ("j613", "15.6", 0, None, "j516s j613", "kernel"),      # listed, other stub
        ]:
            with self.subTest(board=board, stub=stub, try_=try_, pkg=pkg, boards=boards):
                self.mac(board, stub=stub)
                self.assertEqual(self.plan(try_=try_, pkg=pkg, boards=boards), want)

    def test_plan_messages(self):
        self.mac("j613")
        self.assertIn("case D", self.sh("m3_plan").stdout)
        err = self.sh("M3_TRY=1\nm3_plan").stderr
        self.assertIn("in testing", err)
        self.assertIn("desktop keeps", err)

    def test_plan_try_refused(self):
        for board, stub, pkg, why in [("j613", "15.6", None, "stub is 15.6"),
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
                                       ("j516s", 0, None, AURORA8), ("j314s", 0, None, AURORA8)]:
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
        self.assertNotIn(b"t6030", boot.split(b"UBOOT")[-1])
        self.assertNotIn(b"t8122-dcp", boot.split(b"UBOOT")[-1])
        self.assertEqual([d for d in self.downloaded() if d.startswith("m1n1-")], [AURORA8])
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-gpu")
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
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-gpu")


    def test_display_switch(self):
        self.mac("j613")
        self.air_install(try_=1, dcp=1)
        self.assertTrue(self.boot.read_bytes().endswith(b"UBOOT" + AIR_GPU + AIR_DCP))


    def test_refused_with_an_m1n1_that_lacks_the_switch(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        proc = self.air_install(try_=1, pkg=flow.AURORA6, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("no M3 Air GPU handoff", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -U", self.log())

    def test_m3_pro_unchanged_by_air_settings(self):
        # The same m1n1 as the Air, with the M3 Pro's own switches.
        self.mac("j516s")
        out = self.air_install(dcp=1).stdout
        self.assertIn("built-in display at its native resolution", out)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora8-1\n"), boot[:80])
        self.assertTrue(boot.endswith(flow.SWITCHES), boot[-200:])
        self.assertEqual(self.downloaded().count(AURORA8), 1)

    def test_m1_unchanged_by_air_settings(self):
        self.mac("j314s")
        self.air_install(dcp=1)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora8-1\n"), boot[:80])
        self.assertNotIn(b"chosen.", boot)
        self.assertFalse(self.m1n1_conf.exists())



DRY_RUN = shell_value("M3_AIR_DRY_RUN_SWITCHES").split()
DRY_RUN_LINES = b"".join(l.encode() + b"\n" for l in DRY_RUN)


class M3AirDryRunTest(M3AirTest):
    """The Air dry-run profile, and the display handoff that ships with its switches."""
    dry_run = 1

    def setUp(self):
        super().setUp()
        # aurora8 knows every dry-run switch, the handoff's too.
        shutil.rmtree(self.tmp / ("root-" + AURORA8))
        names = [l[len("chosen."):].split("=")[0] for l in DRY_RUN]
        self.fixture(AURORA8, None, ["asahi,t6030-gpu", "asahi,t6030-dcp", "asahi,t6030-dcpext",
                                     "asahi,t8122-gpu"] + names)

    # The inherited handoff tests run with M3_AIR_DRY_RUN=0 in M3AirTest; the
    # ones that only make sense for the handoff's switches are skipped here.
    def test_switches(self):
        self.mac("j613")
        for dcp in (0, 1):
            self.assertEqual(self.sh("m3_switches", dcp=dcp).stdout.split(), DRY_RUN)
        self.mac("j516s")
        self.assertEqual(self.sh("m3_switches").stdout.strip(), shell_value("M3_SWITCHES"))

    def test_shipped_settings(self):
        # A release decision: change this test with it. 12.0 ships the Air
        # display handoff with the dry run's switches, opt-in only, from the
        # same m1n1 as the M3 Pros, under a variant name no test build used.
        self.assertEqual(DRY_RUN, ["chosen.asahi,t8122-gpu-diag=1",
                                   "chosen.asahi,t8122-gpu-handoff-diag=1",
                                   "chosen.asahi,t8122-gpu-power-diag=1",
                                   "chosen.asahi,t8122-dcp=1"])
        # The dry run never asks for the GPU setup itself.
        self.assertNotIn("chosen.asahi,t8122-gpu=1", DRY_RUN)
        self.assertEqual(shell_word("M3_AIR_DRY_RUN"), "1")
        self.assertEqual(shell_word("M3_AIR_DISPLAY_HANDOFF"), "1")
        # One m1n1 for every Mac: no package of the Air's own.
        self.assertNotIn("M3_AIR_M1N1_PACKAGE", SRC)
        self.assertNotIn("M3_M1N1_PACKAGE", SRC)
        self.assertRegex(shell_value("M1N1_PACKAGE"), r"^m1n1-aurora-\S+-aarch64\.pkg\.tar\.zst \S+$")
        # 11.111-test and 11.111.1-test recorded air-display-handoff.
        self.assertNotIn(DISPLAY_VARIANT, ("air-display-handoff", "air-dry-run", "air-gpu", "air-gpu-dcp"))
        self.assertRegex(DISPLAY_VARIANT, r"^air-display-handoff-\S+$")

    def test_conf_block(self):
        self.mac("j613")
        self.sh("m3_switches_write\nm3_switches_write")
        chosen = [l for l in self.m1n1_conf.read_text().splitlines() if l.startswith("chosen.")]
        self.assertEqual(chosen, DRY_RUN)
        self.sh("m3_switches_remove")
        # Nothing else was in it, so the file goes.
        self.assertFalse(self.m1n1_conf.exists())

    def test_conf_block_replaces_an_m3_pro_block(self):
        self.mac("j516s")
        self.sh("m3_switches_write")
        self.mac("j613")
        self.sh("m3_switches_write")
        chosen = [l for l in self.m1n1_conf.read_text().splitlines() if l.startswith("chosen.")]
        self.assertEqual(chosen, DRY_RUN)

    def test_package_check(self):
        pkgs = self.tmp / "pkgs"
        self.mac("j613")
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{pkgs / AURORA8}' && echo yes").stdout.strip(), "yes")
        # An m1n1 with the GPU handoff switch alone can't run the dry run.
        self.fixture("m1n1-gpu-only.pkg.tar.zst", None, ["asahi,t8122-gpu"])
        only = pkgs / "m1n1-gpu-only.pkg.tar.zst"
        self.assertEqual(self.sh(f"m1n1_pkg_has_handoff '{only}' && echo yes || echo no").stdout.strip(), "no")

    def test_verify_bootbin(self):
        self.mac("j613")
        self.boot.write_bytes(b"M1N1:original\nDTBS:x\nUBOOT" + DRY_RUN_LINES)
        (self.fake / "m1n1.bin").write_text("M1N1:original\n")
        self.assertIn("M3 Air handoff switches", self.sh("m3_verify_bootbin").stdout)
        self.boot.write_bytes(b"M1N1:original\nDTBS:x\nUBOOT" + AIR_GPU)
        proc = self.sh("m3_verify_bootbin", check=False)
        self.assertNotEqual(proc.returncode, 0)

    def test_plan_messages(self):
        self.mac("j613")
        self.assertIn("case D", self.sh("m3_plan").stdout)
        err = self.sh("M3_TRY=1\nm3_plan").stderr
        self.assertIn("GPU and display dry run", err)
        self.assertIn("it switches nothing on", err)
        self.assertIn("short identity read", err)

    def test_handoff_and_back(self):
        self.mac("j613")
        proc = self.air_install(try_=1)
        self.assertIn("only\n    reads and reports", proc.stdout)
        self.assertIn("serial recorder", proc.stdout)
        self.assertIn("M3 Air handoff switches", proc.stdout)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora8-1\n"), boot)
        self.assertTrue(boot.endswith(b"UBOOT" + DRY_RUN_LINES), boot)
        self.assertNotIn(b"t8122-gpu=1", boot.split(b"UBOOT")[-1])
        self.assertNotIn(b"t6030", boot.split(b"UBOOT")[-1])
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-dry-run")
        self.sh("uninstall_all")
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-stock\n"), boot)
        self.assertNotIn(b"chosen.", boot)
        self.assertFalse(self.m1n1_conf.exists())

    def test_rerun_keeps_the_handoff(self):
        self.mac("j615")
        self.air_install(try_=1)
        self.assertIn("keeping it", self.air_install().stdout)
        self.assertTrue(self.boot.read_bytes().endswith(DRY_RUN_LINES))

    def test_display_switch(self):
        self.skipTest("the dry run always includes the display part")

    def test_refused_with_an_m1n1_that_lacks_the_switch(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        proc = self.air_install(try_=1, pkg=flow.AURORA6, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("no M3 Air GPU and display dry run", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -U", self.log())

    def test_plan_try_refused(self):
        for board, stub, pkg, why in [("j613", "15.6", None, "stub is 15.6")]:
            with self.subTest(board=board, stub=stub, pkg=pkg):
                self.mac(board, stub=stub)
                proc = self.sh("M3_TRY=1\nm3_plan", pkg=pkg, check=False)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(why, proc.stderr)
                self.assertIn("Nothing was installed", proc.stderr)

    # The recorded variant: a dry run never becomes a GPU start unasked.

    def test_later_gpu_build_needs_a_new_opt_in(self):
        self.mac("j613")
        self.air_install(try_=1)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-dry-run")
        before = self.boot.read_bytes()
        self.dry_run = 0          # a later release that starts the GPU
        proc = self.air_install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("earlier test build's boot loader", proc.stderr)
        self.assertIn("(air-dry-run)", proc.stderr)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        # Asking again switches it.
        self.air_install(try_=1)
        self.assertTrue(self.boot.read_bytes().endswith(b"UBOOT" + AIR_GPU), self.boot.read_bytes())
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-gpu")

    def test_unrecorded_variant_needs_a_new_opt_in(self):
        # A record without a variant (the never-released 6e8493e4) is not kept.
        self.mac("j613")
        self.air_install(try_=1)
        (self.state / "m3-mode").write_text("handoff\n")
        proc = self.air_install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)

    def test_m3_pro_old_record_still_kept(self):
        # 11.36.1 recorded a bare "handoff" on M3 Pros; that still keeps it.
        self.mac("j516s")
        self.air_install()
        (self.state / "m3-mode").write_text("handoff\n")
        out = self.air_install().stdout
        self.assertIn("keeping it", out)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {flow.PRO_VARIANT}")

    def test_plain_rerun_updates_the_dry_run_m1n1(self):
        # Scott's path: an Air on 11.110-test's dry run takes a newer dry-run
        # m1n1 from the next test build's plain one-liner, no flag needed.
        self.mac("j613")
        self.air_install(try_=1)
        newer = "m1n1-aurora-1.6.1.aurora8.1-1-aarch64.pkg.tar.zst"
        names = [l[len("chosen."):].split("=")[0] for l in DRY_RUN]
        self.fixture(newer, None, ["asahi,t8122-gpu"] + names)
        proc = self.air_install(pkg=newer)
        self.assertIn("keeping it", proc.stdout)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora8.1-1\n"), boot)
        self.assertTrue(boot.endswith(b"UBOOT" + DRY_RUN_LINES), boot)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-dry-run")
        # The boot loader the Mac had before the first test stays the kept copy.
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")

    # The display handoff (M3_AIR_DISPLAY_HANDOFF=1)

    def test_display_handoff_needs_fresh_opt_in_and_keeps_restore(self):
        self.mac("j613")
        self.air_install(try_=1)
        before = self.boot.read_bytes()
        before_conf = self.m1n1_conf.read_bytes()
        before_downloads = self.downloaded()
        proc = self.air_install(display=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertIn("display handoff with GPU diagnostics", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertEqual(self.m1n1_conf.read_bytes(), before_conf)
        self.assertEqual(self.downloaded(), before_downloads)
        proc = self.air_install(try_=1, display=1)
        self.assertIn("Native display still needs T8122 PMP support", proc.stderr)
        self.assertIn("does not start the GPU", proc.stderr)
        self.assertIn("Native display and GPU acceleration are not enabled", proc.stdout)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {DISPLAY_VARIANT}")
        self.assertTrue(self.boot.read_bytes().endswith(b"UBOOT" + DRY_RUN_LINES))
        self.assertNotIn(AIR_GPU, self.boot.read_bytes())
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")
        proc = self.air_install(display=1)
        self.assertIn("keeping it", proc.stdout)
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")

    def test_shipped_display_profile_uses_diagnostics_without_gpu_start(self):
        self.mac("j613")
        proc = self.run_sh("M3_TRY=1\nm3_plan\nm3_variant\nm3_switches")
        self.assertIn(DISPLAY_VARIANT, proc.stdout)
        self.assertIn("does not start the GPU", proc.stderr)
        self.assertIn("Native display still needs T8122 PMP support", proc.stderr)
        switches = [s for s in proc.stdout.split() if s.startswith("chosen.")]
        self.assertEqual(switches, DRY_RUN)
        self.assertNotIn("chosen.asahi,t8122-gpu=1", switches)

    def test_release_display_variant_requires_opt_in(self):
        self.mac("j615")
        self.assertEqual(self.plan(display=1), "kernel")
        self.assertEqual(self.plan(try_=1, display=1), "handoff")

    def test_test_build_display_handoff_does_not_move_silently(self):
        # Scott's path: an Air on 11.111.1-test's display handoff (variant
        # air-display-handoff, aurora8.4) runs this release's plain one-liner.
        # It must stop before downloading anything; --m3-handoff moves it.
        self.mac("j613")
        self.air_install(try_=1, display=1, variant="air-display-handoff")
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-display-handoff")
        before = self.boot.read_bytes()
        before_conf = self.m1n1_conf.read_bytes()
        before_downloads = self.downloaded()
        proc = self.air_install(display=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("(air-display-handoff)", proc.stderr)
        self.assertIn(f"({DISPLAY_VARIANT})", proc.stderr)
        self.assertIn("--m3-handoff", proc.stderr)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertEqual(self.m1n1_conf.read_bytes(), before_conf)
        self.assertEqual(self.downloaded(), before_downloads)
        proc = self.air_install(try_=1, display=1)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {DISPLAY_VARIANT}")
        self.assertTrue(self.boot.read_bytes().endswith(b"UBOOT" + DRY_RUN_LINES))
        # The boot loader the Mac had before its first test stays the kept copy.
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")

    def test_display_handoff_needs_the_checked_stage1(self):
        # Scott's Air on another m1n1 stage 1: --m3-handoff stops with nothing
        # downloaded, and a plain run stays kernel-only.
        self.mac("j613", stage1="v1.7.0")
        before = self.boot.read_bytes()
        proc = self.air_install(try_=1, display=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("stage 1 is v1.7.0", proc.stderr)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertEqual(self.downloaded(), [])
        self.assertEqual(self.plan(display=1), "kernel")

    def test_display_handoff_air_on_another_stage1_keeps_what_it_has(self):
        # Scott's Air with this release's display handoff, after its stage 1
        # changed: a plain run stops before any download and names no flag.
        self.mac("j613")
        self.air_install(try_=1, display=1)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {DISPLAY_VARIANT}")
        self.mac("j613", bootbin=self.boot.read_bytes(), stage1="v1.7.0")
        before = self.boot.read_bytes()
        before_downloads = self.downloaded()
        proc = self.air_install(display=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        err = " ".join(proc.stderr.split())
        self.assertIn("stage 1 is v1.7.0", err)
        self.assertIn("this Mac keeps the boot loader and kernel it has", err)
        self.assertIn("issues/6", err)
        self.assertNotIn("--m3-handoff", err)
        self.assertNotIn("keeping it", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertEqual(self.downloaded(), before_downloads)

    def test_kept_air_gets_no_new_opt_in_warning(self):
        self.mac("j613")
        self.air_install(try_=1, display=1)
        proc = self.air_install(display=1)
        self.assertIn("keeping it", proc.stdout)
        self.assertNotIn("--m3-handoff: trying", proc.stderr)

    def test_install_syncs_after_the_rebuild(self):
        src = SRC[SRC.index("      m3_verify_bootbin\n"):]
        self.assertTrue(re.match(r"      m3_verify_bootbin\n      m1n1_check_and_record\n(\s*#.*\n)*\s*sync\n", src),
                        src[:200])

if __name__ == "__main__":
    unittest.main()
