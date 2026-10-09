"""The M3 MacBook Air's default handoff, against the fake Mac of test_m3_flow.

The J613 is in M3_HANDOFF_BOARDS. With the shipped settings a plain run gives it this release's
m1n1 with M3_AIR_SWITCHES: m1n1's display handoff (chosen.asahi,t8122-dcp=1) and the GPU firmware
description (chosen.asahi,t8122-gpu-handoff-diag=1). The GPU diagnostics stay opt-in
(--m3-handoff gives M3_AIR_DRY_RUN_SWITCHES, as 12.3 does), nothing writes the GPU start
(chosen.asahi,t8122-gpu=1), and the J615 stays kernel-only unless asked. An owner switches an Air
switch off on their Mac with a chosen.<name>=0 line of their own in /etc/m1n1.conf.

SameAs123Test runs 12.3's script and this one on every other board, and on a J613 with
--m3-handoff: the same commands, the same files and the same output.
"""
import re
import unittest

import test_m3_flow as flow
import test_m3_pro_mesa as pro

SRC = flow.SRC
REL_12_3 = "4f2a68f8"


def normalize_undo_label(text):
    # The uninstall command is unchanged; its label now describes the kernel install.
    return re.sub(r"(?m)^   To undo everything:    ", "   To undo the kernel install: ", text)


class UndoLabelTest(unittest.TestCase):
    def test_normalization_preserves_command_and_url_differences(self):
        command = "curl -fsSL https://example.test/install.sh | bash -s -- --uninstall\n"
        old = "   To undo everything:    " + command
        current = "   To undo the kernel install: " + command
        self.assertEqual(normalize_undo_label(old), current)
        for changed in (current.replace("--uninstall", "--reset-touchid"),
                        current.replace("example.test", "other.test"),
                        current.replace("bash -s --", "bash")):
            self.assertNotEqual(normalize_undo_label(old), normalize_undo_label(changed))


def shell_value(name):
    return re.search(rf'^{name}="([^"]*)"$', SRC, re.M).group(1)


DEFAULT = shell_value("M3_AIR_SWITCHES").split()
DIAG = shell_value("M3_AIR_DRY_RUN_SWITCHES").split()
DEFAULT_VARIANT = shell_value("M3_AIR_DEFAULT_VARIANT")
DISPLAY_VARIANT = shell_value("M3_AIR_DISPLAY_VARIANT")
DCP = "chosen.asahi,t8122-dcp=1"
GPU_DESC = "chosen.asahi,t8122-gpu-handoff-diag=1"


def lines(switches):
    return b"".join(s.encode() + b"\n" for s in switches)


class AirDefaultTest(flow.M3FlowBase):
    def chosen(self):
        return [l for l in self.m1n1_conf.read_text().splitlines() if l.startswith("chosen.")]

    def tail(self):
        return self.boot.read_bytes().split(b"UBOOT")[-1]

    def mode(self):
        return (self.state / "m3-mode").read_text().strip()

    # What ships

    def test_shipped_profile(self):
        # A release decision: change this test with it.
        self.assertEqual(DEFAULT, [DCP, GPU_DESC])
        self.assertIn("j613", shell_value("M3_HANDOFF_BOARDS").split())
        self.assertNotIn("j615", shell_value("M3_HANDOFF_BOARDS").split())
        # The diagnostics and the GPU start are not in it.
        for s in ("chosen.asahi,t8122-gpu-diag=1", "chosen.asahi,t8122-gpu-power-diag=1",
                  "chosen.asahi,t8122-gpu=1", "chosen.asahi,t8122-gpu-power-standin=1"):
            self.assertNotIn(s, DEFAULT)
        # The --m3-handoff profile keeps 12.3's switches and name.
        self.assertEqual(DIAG, ["chosen.asahi,t8122-gpu-diag=1", "chosen.asahi,t8122-gpu-handoff-diag=1",
                                "chosen.asahi,t8122-gpu-power-diag=1", DCP])
        self.assertEqual(DISPLAY_VARIANT, "air-display-handoff-12")
        self.assertNotEqual(DEFAULT_VARIANT, DISPLAY_VARIANT)
        # The shipped m1n1 knows every switch of both profiles.
        names = {s[len("chosen."):].split("=")[0] for s in DEFAULT + DIAG}
        self.assertLessEqual(names, set(flow.SWITCH_NAMES))

    # The J613's plain run

    def test_j613_plain_run_gets_the_default_and_back(self):
        self.mac("j613")
        proc = self.install()
        self.assertIn("M3 MacBook Air (j613, macOS 14.8.3 stub): installing m1n1 with the M3 Air display handoff",
                      proc.stdout)
        self.assertIn("m1n1's boot.bin is this release's m1n1 with the M3 Air handoff switches", proc.stdout)
        self.assertIn("nothing\n    starts the GPU", proc.stdout)
        self.assertIn("chosen.asahi,t8122-dcp=0 for the display handoff", proc.stdout)
        self.assertNotIn("--m3-handoff: trying", proc.stderr)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:" + flow.M1N1_BASE.encode() + b"\n"), boot[:80])
        self.assertEqual(self.tail(), lines(DEFAULT))
        self.assertEqual(self.chosen(), DEFAULT)
        self.assertIn("# >>> aurora-sep: M3 Air handoff (remove with: install-aurora-sep.sh --uninstall)",
                      self.m1n1_conf.read_text())
        self.assertEqual(self.mode(), f"handoff {DEFAULT_VARIANT}")
        self.assertEqual([d for d in self.downloaded() if d.startswith("m1n1-")], [flow.M1N1_PKG])
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")
        self.assertNotIn("M1N1_UPDATE_DISABLED", self.update_conf.read_text())

        # A plain rerun keeps it as it is.
        proc = self.install()
        self.assertIn("keeping it", proc.stdout)
        self.assertEqual(self.tail(), lines(DEFAULT))
        self.assertEqual(self.mode(), f"handoff {DEFAULT_VARIANT}")
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")

        self.uninstall()
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-stock\n"), boot)
        self.assertNotIn(b"chosen.", boot)
        self.assertFalse(self.m1n1_conf.exists())
        self.assertFalse(self.state.exists())

    def test_j613_on_another_stub_or_stage1_stays_kernel_only(self):
        for kw in ({"stub": "15.6"}, {"stage1": "v1.7.0"}, {"stage1": None}):
            with self.subTest(**kw):
                self.fresh_state()
                self.mac("j613", **kw)
                before = self.boot.read_bytes()
                proc = self.install()
                self.assertIn("Installing the kernel only", proc.stderr)
                self.assertEqual(self.boot.read_bytes(), before)
                self.assertFalse([d for d in self.downloaded() if d.startswith("m1n1-")])
                self.assertEqual(self.mode(), "kernel")

    def test_j615_stays_kernel_only(self):
        self.mac("j615")
        before = self.boot.read_bytes()
        out = self.install().stdout
        self.assertIn("installing the kernel only", out)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertEqual(self.mode(), "kernel")

    # The diagnostics stay opt-in

    def test_j613_m3_handoff_adds_the_diagnostics(self):
        self.mac("j613")
        proc = self.install(try_=1)
        self.assertIn("--m3-handoff: trying m1n1's display handoff on this M3 MacBook Air (j613)", proc.stderr)
        self.assertIn("collects GPU diagnostics", proc.stderr)
        self.assertEqual(self.tail(), lines(DIAG))
        self.assertEqual(self.mode(), f"handoff {DISPLAY_VARIANT}")
        # Asked once, kept on a plain rerun.
        proc = self.install()
        self.assertIn("keeping it", proc.stdout)
        self.assertEqual(self.tail(), lines(DIAG))
        self.assertEqual(self.mode(), f"handoff {DISPLAY_VARIANT}")

    def test_j613_default_then_m3_handoff(self):
        self.mac("j613")
        self.install()
        self.install(try_=1)
        self.assertEqual(self.tail(), lines(DIAG))
        self.assertEqual(self.mode(), f"handoff {DISPLAY_VARIANT}")
        # The boot loader from before the first install stays the kept copy.
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")

    def test_no_variant_writes_the_gpu_start(self):
        for board, try_ in (("j613", 0), ("j613", 1), ("j615", 1)):
            with self.subTest(board=board, try_=try_):
                self.fresh_state()
                self.mac(board)
                self.install(try_=try_)
                self.assertNotIn(b"t8122-gpu=1", self.tail())
                self.assertNotIn(b"t8122-gpu-power-standin", self.tail())

    # Earlier installs

    def test_j613_on_an_earlier_test_build_moves_with_the_release(self):
        # A listed Air moves as a listed M3 Pro does: an 11.111.1-test display handoff
        # (variant air-display-handoff) or an 11.110-test dry run takes the default profile.
        for old in ("air-display-handoff", "air-dry-run"):
            with self.subTest(old=old):
                self.fresh_state()
                self.mac("j613", bootbin=b"M1N1:test-build\nDTBS:x\nUBOOT" + lines(DIAG))
                (self.state / "m3-mode").write_text(f"handoff {old}\n")
                proc = self.install()
                self.assertNotIn("Nothing was installed", proc.stderr)
                self.assertEqual(self.tail(), lines(DEFAULT))
                self.assertEqual(self.mode(), f"handoff {DEFAULT_VARIANT}")

    def test_j613_from_12_3(self):
        # 12.3's script: a J613 kernel-only, then this release's plain run gives it the default;
        # a J613 that opted in on 12.3 keeps 12.3's switches.
        old, _ = pro.old_installer(self, REL_12_3, "install-12.3.sh")

        def install_123(try_):
            self.installer = old
            try:
                return self.run_sh(f"M3_TRY={try_}\ninstall_all")
            finally:
                self.installer = flow.INSTALLER

        self.mac("j613")
        install_123(0)
        self.assertEqual(self.mode(), "kernel")
        self.install()
        self.assertEqual(self.tail(), lines(DEFAULT))
        self.assertEqual(self.mode(), f"handoff {DEFAULT_VARIANT}")

        self.fresh_state()
        self.mac("j613")
        install_123(1)
        self.assertEqual(self.mode(), f"handoff {DISPLAY_VARIANT}")
        conf = self.m1n1_conf.read_bytes()
        boot = self.boot.read_bytes()
        proc = self.install()
        self.assertIn("keeping it", proc.stdout)
        self.assertEqual(self.m1n1_conf.read_bytes(), conf)
        self.assertEqual(self.boot.read_bytes(), boot)
        self.assertEqual(self.mode(), f"handoff {DISPLAY_VARIANT}")

    # The per-Mac off switch in /etc/m1n1.conf

    def test_off_switch(self):
        self.mac("j613")
        self.m1n1_conf.write_text("chosen.asahi,t8122-dcp=0\n")
        proc = self.install()
        self.assertIn("Switched off on this Mac in", proc.stdout)
        self.assertIn("chosen.asahi,t8122-dcp\n", proc.stdout.replace(" ", "\n"))
        # The owner's 0 stays, and the block leaves the switch out.
        self.assertEqual(self.chosen(), ["chosen.asahi,t8122-dcp=0", GPU_DESC])
        self.assertEqual(self.tail(), b"chosen.asahi,t8122-dcp=0\n" + lines([GPU_DESC]))
        self.assertNotIn(DCP.encode(), self.boot.read_bytes())
        # A rerun keeps it off; --uninstall leaves the owner's line.
        self.install()
        self.assertEqual(self.chosen(), ["chosen.asahi,t8122-dcp=0", GPU_DESC])
        self.uninstall()
        self.assertEqual(self.m1n1_conf.read_text(), "chosen.asahi,t8122-dcp=0\n")

    def test_off_switch_added_after_the_install(self):
        # The summary's steps: the line at the end, then update-m1n1. m1n1 takes a chosen line's
        # last value, so the 0 wins at once; the next run drops the 1.
        self.mac("j613")
        self.install()
        with open(self.m1n1_conf, "a") as f:
            f.write("chosen.asahi,t8122-gpu-handoff-diag=0\n")
        self.run_sh("update-m1n1")
        self.assertTrue(self.boot.read_bytes().endswith(b"chosen.asahi,t8122-gpu-handoff-diag=0\n"))
        self.install()
        # The owner's line stays where it is; the block goes after it, without the switch.
        self.assertEqual(self.chosen(), ["chosen.asahi,t8122-gpu-handoff-diag=0", DCP])
        self.assertNotIn(GPU_DESC.encode(), self.boot.read_bytes())
        # Deleting the line switches it back on with the next run.
        text = self.m1n1_conf.read_text().replace("chosen.asahi,t8122-gpu-handoff-diag=0\n", "")
        self.m1n1_conf.write_text(text)
        self.install()
        self.assertEqual(self.chosen(), DEFAULT)

    def test_everything_off(self):
        self.mac("j613")
        self.m1n1_conf.write_text("chosen.asahi,t8122-dcp=0\nchosen.asahi,t8122-gpu-handoff-diag=0\n")
        proc = self.install()
        self.assertIn("m1n1's boot.bin is this release's m1n1 with the M3 Air handoff switches", proc.stdout)
        self.assertNotIn(b"=1", self.tail())
        self.assertEqual(self.mode(), f"handoff {DEFAULT_VARIANT}")

    def test_only_an_exact_zero_line_is_an_off_switch(self):
        for line in ("# chosen.asahi,t8122-dcp=0", "chosen.asahi,t8122-dcp=00", "chosen.asahi,t8122-dcp = 0",
                     "chosen.asahi,t8122-dcp=no"):
            with self.subTest(line=line):
                self.fresh_state()
                self.mac("j613")
                self.m1n1_conf.write_text(line + "\n")
                self.install()
                self.assertIn(DCP, self.chosen())

    def test_off_switch_on_the_diagnostics_profile(self):
        self.mac("j615")
        self.m1n1_conf.write_text("chosen.asahi,t8122-gpu-power-diag=0\n")
        proc = self.install(try_=1)
        self.assertIn("Switched off on this Mac in", proc.stdout)
        want = [s for s in DIAG if s != "chosen.asahi,t8122-gpu-power-diag=1"]
        self.assertEqual(self.chosen(), ["chosen.asahi,t8122-gpu-power-diag=0"] + want)
        self.assertNotIn(b"t8122-gpu-power-diag=1", self.boot.read_bytes())

    def test_off_lines_never_change_an_m3_pro(self):
        # The M3 Pro's three switches are written as in 12.3, whatever the file holds.
        self.mac("j516s")
        self.m1n1_conf.write_text("chosen.asahi,t6030-dcp=0\nchosen.asahi,t8122-dcp=0\n")
        self.install()
        self.assertEqual(self.chosen(), ["chosen.asahi,t6030-dcp=0", "chosen.asahi,t8122-dcp=0"]
                         + shell_value("M3_SWITCHES").split())
        self.assertTrue(self.boot.read_bytes().endswith(flow.SWITCHES))


# 12.4's recovery text names the hook's reasons as mesa-m3 26.1.4.m3.1-6 does: not-t6030 is
# now not-supported, and experimental is new (the M3 Air's GPU without its opt-in).
REASONS_123 = ("   Reasons: active, opt-out, not-t6030, no-gpu, no-display, no-access, incomplete-prefix,\n"
               "   user-setup, user-setup-ldpath, user-setup-unknown, missing-soname, load-failed,\n"
               "   log-unreadable, gpu-fault, previous-failed. When off, the desktop renders in software.\n")
REASONS_124 = ("   Reasons: active, opt-out, not-supported, experimental, no-gpu, no-display, no-access,\n"
               "   incomplete-prefix, user-setup, user-setup-ldpath, user-setup-unknown, missing-soname,\n"
               "   load-failed, log-unreadable, gpu-fault, previous-failed. When off, the desktop renders in software.\n")
AIRS = re.search(r'^M3_AIR_BOARDS="([^"]*)"$', flow.SRC, re.M).group(1).split()


class SameAs123Test(flow.M3FlowBase):
    """12.3's script and this one on every board but the J613's plain run: the same commands in
    the same order, the same files, the same exit status and the same output. The M3 Pro's output
    differs only in the recovery text's reason names (REASONS_124) and the undo label. The M3 Airs, which get mesa-m3
    from 12.4 on (test_m3_air_mesa), run here without a mesa-m3 entry: everything else of theirs,
    m1n1's handoff above all, is as on 12.3."""

    def setUp(self):
        super().setUp()
        self.old, self.old_version = pro.old_installer(self, REL_12_3, "install-12.3.sh")

    # What may differ between two runs of the same script: mktemp names and the M3 Pro's Mesa
    # record's run binding (its run id and time, and the installer's own sha256).
    TEMP = re.compile(r"(\.m3-pro-mesa\.)[A-Za-z0-9]{6}")
    RUN_ID = re.compile(r"run_id=[0-9a-f-]{36}")
    RECORD_RUN = re.compile(rb"(?m)^(run_id|written_at|boot_id|installer_sha256)=.*\n")

    def norm(self, text):
        text = pro.refresh_notice(normalize_undo_label(text))
        text = text.replace(REASONS_124, REASONS_123)
        return self.RUN_ID.sub("run_id=<id>", self.TEMP.sub(r"\1<tmp>", pro.MKTEMP.sub("<tmp>", text)))

    def snapshot(self):
        tree = pro.tree(self)
        if "state/m3-pro-mesa" in tree:
            tree["state/m3-pro-mesa"] = self.RECORD_RUN.sub(b"", tree["state/m3-pro-mesa"])
        return tree

    def run_pair(self, board, try_, setup=None):
        no_mesa = 'M3_PRO_MESA_PACKAGE=""\n' if board in AIRS else ""

        def run():
            a = self.run_sh(pro.SUDO_LOG + no_mesa + f"M3_TRY={try_}\ninstall_all", check=False)
            installed.append(self.snapshot())
            b = self.run_sh(pro.SUDO_LOG + no_mesa + "uninstall_all", check=False) if a.returncode == 0 else None
            outs.append(tuple(self.norm(x) if x else x for x in (a.stdout, a.stderr, b and b.stdout,
                                                                 b and b.stderr)))
            return (a.returncode, b and b.returncode)

        outs, installed = [], []
        before = pro.run_with(self, self.old, board, run, setup)
        after = pro.run_with(self, flow.INSTALLER, board, run, setup)
        # 12.3's runs with its release number read as this one's: a release names its own packages,
        # tag and boot.bin copy, and nothing else may differ.
        if self.old_version != flow.VERSION:
            before = pro.as_this_release(before, self.old_version, flow.VERSION)
            installed[0] = {k.replace(self.old_version, flow.VERSION):
                            v.replace(self.old_version.encode(), flow.VERSION.encode())
                            for k, v in installed[0].items()}
            # The kernel release the NEXT STEPS box names follows VERSION too (7.1.12-2-<rel>-sep-ARCH).
            krel = lambda v: f"-2-{v.rsplit('-', 1)[1]}-sep-ARCH"
            outs[0] = tuple(x.replace(self.old_version, flow.VERSION).replace(krel(self.old_version),
                                                                          krel(flow.VERSION)) if x else x
                            for x in outs[0])
        self.assertEqual(after["codes"], before["codes"])
        pro.same_commands(self, self.norm(before["log"]), self.norm(after["log"]))
        # The files after the install, and after the uninstall.
        for old, new in ((installed[0], installed[1]), (before["tree"], after["tree"])):
            self.assertEqual(sorted(new), sorted(old))
            for path, data in old.items():
                self.assertEqual(new[path], data, path)
        self.assertEqual(outs[1], outs[0])
        return after

    def test_every_other_board(self):
        seen = []
        for board in flow.BOARDS:
            for try_ in (0, 1):
                if board == "j613" and try_ == 0:
                    continue
                with self.subTest(board=board, try_=try_):
                    self.run_pair(board, try_)
                    seen.append((board, try_))
        for want in (("j516s", 0), ("j514s", 1), ("j615", 0), ("j615", 1), ("j613", 1), ("j504", 0),
                     ("j516m", 0), ("j314s", 0), ("j293", 0), ("j414s", 0), ("j700", 0)):
            self.assertIn(want, seen)

    def test_an_air_with_an_earlier_opt_in(self):
        # A J615 (and a J613) that opted in on 12.3 runs the plain one-liner: as on 12.3. The opt-in
        # is made under an earlier release number, so both one-liners are a new release there (each
        # keeps its own boot.bin copy), as 12.3's own one-liner was before 12.4 existed.
        for board in ("j615", "j613"):
            with self.subTest(board=board):
                def setup():
                    self.installer = self.old
                    try:
                        self.run_sh("VERSION=7.1.12.aurora2-12.2.99\nTAG=sep-7.1.12.aurora2-12.2.99\n"
                                    "M3_TRY=1\ninstall_all")
                    finally:
                        self.installer = flow.INSTALLER
                    (self.fake / "log").write_text("")
                self.run_pair(board, 0, setup)


if __name__ == "__main__":
    unittest.main()
