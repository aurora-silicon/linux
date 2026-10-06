"""A boot loader that failed is never put back (the restore loop).

After an install, a Mac that stops in m1n1 gets its old boot.bin back from
macOS with the printed steps, then two printed lines: a freeze on
update-m1n1, and a line that records the failed m1n1 in $STATE/m1n1-failed.
The installer also records it when it finds boot.bin put back by hand. Later
runs keep the boot loader the Mac has, and never install that m1n1 again.
Runs against the fake Mac of test_m3_flow.
"""
import unittest

import test_m3_flow as flow

FREEZE = "M1N1_UPDATE_DISABLED=1\n"


class RollbackTest(flow.M3FlowBase):
    def setUp(self):
        super().setUp()
        self.sha = self.bin_shas[self.m1n1_pkg]

    def m1n1_downloads(self, since=""):
        return [l for l in self.log()[len(since):].splitlines() if l.startswith("curl ") and "/m1n1-" in l]

    def failed(self):
        path = self.state / "m1n1-failed"
        return [l.split()[0] for l in path.read_text().splitlines()] if path.exists() else []

    def put_back(self, freeze=True):
        # The printed macOS steps, then the printed freeze line.
        self.boot.write_bytes(self.kept_copy().read_bytes())
        if freeze:
            with open(self.update_conf, "a") as f:
                f.write(FREEZE)

    def test_restore_steps_name_the_m1n1_and_the_issue(self):
        for board, try_ in (("j516s", 0), ("j314s", 0), ("j514s", 1)):
            with self.subTest(board=board):
                self.fresh_state()
                self.mac(board)
                err = self.install(try_=try_).stderr
                self.assertIn(f"echo {self.sha} | sudo tee -a {self.state}/m1n1-failed", err)
                self.assertIn(f"echo M1N1_UPDATE_DISABLED=1 | sudo tee -a {self.update_conf}", err)
                self.assertIn(f"sudo cp '{self.kept_copy()}' '{self.boot}.new' && sync && "
                              f"sudo mv '{self.boot}.new' '{self.boot}' && sync", err)
                self.assertIn("https://github.com/iconidentify/aurora-linux/issues/6", err)

    def test_j516s_put_back_and_frozen_keeps_its_boot_loader(self):
        # The review's loop: the J516S stops, the owner restores the kept copy
        # and freezes updates. No later run puts that m1n1 back.
        self.mac("j516s", bootbin=b"M1N1:aurora7\nDTBS:x\nUBOOT" + flow.SWITCHES)
        self.install()
        self.put_back()
        restored = self.boot.read_bytes()
        log = self.log()
        proc = self.install()
        self.assertIn("failed on this Mac before", proc.stdout)
        self.assertIn("issues/6", proc.stdout)
        self.assertNotIn("Remove that line", proc.stdout + proc.stderr)
        self.assertIn("keeps the boot loader it has now", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertEqual(self.m1n1_downloads(log), [])
        self.assertEqual(self.failed(), [self.sha])
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")

        # The owner lifts the freeze anyway: still kept, and frozen again.
        self.update_conf.write_text(self.update_conf.read_text().replace(FREEZE, ""))
        log = self.log()
        self.install()
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertEqual(self.m1n1_downloads(log), [])
        self.assertIn("M1N1_UPDATE_DISABLED=1", self.update_conf.read_text())

        # Asking for the handoff again does not bring that m1n1 back.
        log = self.log()
        proc = self.install(try_=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("is the one that failed", proc.stderr)
        self.assertIn("without --m3-handoff", proc.stderr)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertNotIn("curl ", self.log()[len(log):])

        # --uninstall keeps the record, and a new install keeps the stock boot.bin.
        self.uninstall()
        self.assertEqual(self.failed(), [self.sha])
        stock = self.boot.read_bytes()
        log = self.log()
        self.install()
        self.assertEqual(self.boot.read_bytes(), stock)
        self.assertEqual(self.m1n1_downloads(log), [])

    def test_the_printed_line_alone_records_it(self):
        # WRONG from Linux: the owner runs the printed lines; nothing to detect.
        self.mac("j516s")
        self.install()
        self.put_back()
        with open(self.state / "m1n1-failed", "a") as f:
            f.write(self.sha + "\n")
        restored = self.boot.read_bytes()
        proc = self.install()
        self.assertIn("failed on this Mac before", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertEqual(self.failed(), [self.sha])

    def test_put_back_before_12_0_kept_by_its_freeze(self):
        # An earlier release recorded no m1n1: the freeze from the restore
        # steps alone keeps the boot loader, without "remove that line".
        self.mac("j516s")
        self.install()
        (self.state / "m1n1-installed").unlink()
        self.put_back()
        restored = self.boot.read_bytes()
        proc = self.install()
        self.assertIn("not by this script (the restore steps add that line)", proc.stdout)
        self.assertNotIn("Remove that line", proc.stdout + proc.stderr)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertEqual(self.failed(), [])

    def test_m1_put_back_keeps_its_boot_loader(self):
        # M1 and M2 get the same m1n1, so the same loop is closed there.
        self.mac("j314s")
        self.install()
        self.put_back()
        restored = self.boot.read_bytes()
        log = self.log()
        proc = self.install()
        self.assertIn("failed on this Mac before", proc.stdout)
        self.assertIn("boot.bin is unchanged", proc.stdout)
        self.assertIn("keeps the boot loader it has", proc.stdout)
        self.assertIn("run:  aurora-touchid-setup", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertEqual(self.m1n1_downloads(log), [])
        self.assertIn("linux-aurora", self.log()[len(log):])
        self.assertEqual(self.failed(), [self.sha])

    def test_m1_put_back_without_the_freeze_stops_first(self):
        self.mac("j314s")
        self.install()
        self.put_back(freeze=False)
        restored = self.boot.read_bytes()
        log = self.log()
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertIn(f"echo M1N1_UPDATE_DISABLED=1 | sudo tee -a {self.update_conf}", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertNotIn("pacman -U", self.log()[len(log):])
        self.assertEqual(self.failed(), [self.sha])

    def test_rebuilds_are_not_taken_for_a_put_back(self):
        # A re-run, and a newer m1n1 that the hook built before its check
        # ran, leave boot.bin starting with an m1n1 this script installed.
        self.mac("j314s")
        self.install()
        self.install()
        self.assertEqual(self.failed(), [])
        newer = "m1n1-aurora-1.6.1.aurora13-1-aarch64.pkg.tar.zst"
        self.fixture(newer, None, flow.SWITCH_NAMES)
        self.run_sh(f"pacman -U '{self.tmp / 'pkgs' / newer}'\nm1n1_rollback_check")
        self.assertTrue(self.boot.read_bytes().startswith(b"M1N1:m1n1-aurora-1.6.1.aurora13-1\n"))
        self.assertEqual(self.failed(), [])

    def test_a_newer_m1n1_after_a_failed_one_needs_the_flag(self):
        self.mac("j516s")
        self.install()
        self.put_back()
        self.install()
        self.assertEqual(self.failed(), [self.sha])
        # The next release ships another m1n1.
        newer = "m1n1-aurora-1.6.1.aurora13-1-aarch64.pkg.tar.zst"
        self.fixture(newer, None, flow.SWITCH_NAMES)
        self.m1n1_pkg = newer
        restored = self.boot.read_bytes()
        self.install()
        self.assertEqual(self.boot.read_bytes(), restored)
        # Asked for, it is installed once the owner lifts their freeze.
        proc = self.install(try_=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Remove that line and run this again with --m3-handoff", proc.stderr)
        self.update_conf.write_text(self.update_conf.read_text().replace(FREEZE, ""))
        self.install(try_=1)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora13-1\n"), boot[:80])
        self.assertTrue(boot.endswith(flow.SWITCHES))

    def test_agent_prompt_steps_match_the_printed_ones(self):
        self.mac("j516s")
        prompt = self.run_sh("agent_prompt").stdout
        self.assertIn("IF THE MAC STOPS IN m1n1 AFTER AN INSTALL (any Mac)", prompt)
        self.assertIn("runs the two printed lines", prompt)
        self.assertEqual(prompt.lower().count('follow "if the mac stops in m1n1" above'), 3)
        self.assertNotIn("it puts the stock m1n1 back", prompt)
        self.assertNotIn("Do not run the one-liner again", prompt)


if __name__ == "__main__":
    unittest.main()
