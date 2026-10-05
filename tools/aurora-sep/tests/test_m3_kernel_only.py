"""The installer's kernel-only M3 path, exercised on fixture files.

Each case sources install-aurora-sep.sh for its functions only
(AURORA_SEP_SOURCE_ONLY=1), points the paths it touches at a temporary
directory, and replaces sudo and the EFI-partition lookup.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parent.parent / "install-aurora-sep.sh"

BRINGUP_FREEZE = (
    "# Added by m3-gpu-work/scripts/install-m3gpu.sh: keep the M3 GPU candidate boot.bin.\n"
    "# Undo with rollback-m3gpu.sh (then run: sudo update-m1n1).\n"
    "M1N1_UPDATE_DISABLED=1\n"
)
# What m1n1_update writes on M1/M2 (11.35).
AURORA_DTBS = (
    "# aurora-sep: build m1n1's stage 2 from the device trees the installed\n"
    "DTBS=$(pacman -Qlq linux-aurora 2>/dev/null | grep '/dtbs/[^/]*\\.dtb$'; true)\n"
)
COMPATIBLE = {
    "j516s": ["apple,j516s", "apple,t6030"],
    "j514s": ["apple,j514s", "apple,t6030"],
    "j514c": ["apple,j514c", "apple,t6031"],
    "j514m": ["apple,j514m", "apple,t6034"],
    "j613": ["apple,j613", "apple,t8122"],
    "j314s": ["apple,j314s", "apple,t6000"],
    "j293": ["apple,j293", "apple,t8103"],
    "j700": ["apple,j700", "apple,t8140"],
}


class M3KernelOnlyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.dt = self.tmp / "dt"
        self.dt.mkdir()
        self.conf = self.tmp / "update-m1n1"
        self.boot = self.tmp / "esp" / "m1n1" / "boot.bin"
        self.boot.parent.mkdir(parents=True)
        self.boot.write_bytes(b"m1n1+dtbs+u-boot\nchosen.asahi,t6030-gpu=1\n")
        # A pacman that owns linux-aurora's device trees, as on a Mac after
        # the install: the DTBS= line m1n1_update writes needs one, because
        # update-m1n1 sources it under sh's set -e.
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        (self.bin / "pacman").write_text(
            "#!/bin/sh\necho /usr/lib/modules/7.1.12-2-11.36-sep-ARCH/dtbs/t6030-j516s.dtb\n")
        (self.bin / "pacman").chmod(0o755)

    def mac(self, board):
        (self.dt / "compatible").write_bytes(
            b"".join(c.encode() + b"\0" for c in COMPATIBLE[board] + ["apple,arm-platform"])
        )

    def run_sh(self, body, check=True):
        script = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
sudo=""
DT='{self.dt}'
UPDATE_M1N1_CONF='{self.conf}'
m3_bootbin() {{ [[ -f '{self.boot}' ]] && echo '{self.boot}'; }}
{body}
"""
        proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                              env={**os.environ, "NO_COLOR": "1",
                                   "PATH": f"{self.bin}:{os.environ['PATH']}"})
        if check:
            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        return proc

    def frozen(self):
        return self.run_sh("update_m1n1_frozen && echo yes || echo no").stdout.strip()

    def test_m3_boards(self):
        for board, want in [("j516s", "m3"), ("j514s", "m3"), ("j514c", "m3"), ("j514m", "m3"),
                            ("j613", "m3"), ("j314s", "no"), ("j293", "no"), ("j700", "no")]:
            with self.subTest(board=board):
                self.mac(board)
                self.assertEqual(self.run_sh("is_m3 && echo m3 || echo no").stdout.strip(), want)

    def test_neo_is_not_m3_and_still_neo(self):
        self.mac("j700")
        self.assertEqual(self.run_sh("is_neo && echo neo").stdout.strip(), "neo")

    def test_frozen_like_update_m1n1(self):
        for text, want in [(BRINGUP_FREEZE, "yes"), ("M1N1_UPDATE_DISABLED=\n", "no"),
                           ("# M1N1_UPDATE_DISABLED=1\n", "no"), (AURORA_DTBS, "no"),
                           (AURORA_DTBS + "M1N1_UPDATE_DISABLED=1\n", "yes"),
                           ("export M1N1_UPDATE_DISABLED=y\n", "yes")]:
            with self.subTest(text=text):
                self.conf.write_text(text)
                self.assertEqual(self.frozen(), want)
        self.conf.unlink()
        self.assertEqual(self.frozen(), "no")

    def test_freeze_creates_the_file(self):
        out = self.run_sh("m3_freeze\necho $M3_FROZEN_BY").stdout.strip()
        self.assertEqual(out, "aurora-sep")
        self.assertEqual(self.frozen(), "yes")
        self.run_sh("m3_unfreeze")
        self.assertFalse(self.conf.exists())

    def test_freeze_keeps_existing_settings(self):
        self.conf.write_text(AURORA_DTBS)
        self.run_sh("m3_freeze")
        self.assertTrue(self.conf.read_text().startswith(AURORA_DTBS))
        self.assertEqual(self.frozen(), "yes")
        self.run_sh("m3_unfreeze\nm3_unfreeze")
        self.assertEqual(self.conf.read_text(), AURORA_DTBS)

    def test_bringup_freeze_left_alone(self):
        self.conf.write_text(BRINGUP_FREEZE)
        out = self.run_sh("m3_freeze\necho $M3_FROZEN_BY").stdout.strip()
        self.assertEqual(out, "already")
        self.assertEqual(self.conf.read_text(), BRINGUP_FREEZE)
        self.run_sh("m3_unfreeze")
        self.assertEqual(self.conf.read_text(), BRINGUP_FREEZE)

    def test_freeze_twice_is_one_block(self):
        self.run_sh("m3_freeze\nm3_freeze")
        self.assertEqual(self.conf.read_text().count("M1N1_UPDATE_DISABLED=1"), 1)

    def test_report_unchanged(self):
        self.conf.write_text(BRINGUP_FREEZE)
        out = self.run_sh('M3_BOOTBIN_SHA=$(m3_bootbin_sha)\nm3_freeze\nm3_bootbin_report').stdout
        self.assertIn("boot.bin is unchanged", out)
        self.assertIn("already frozen", out)

    def test_report_ours(self):
        out = self.run_sh('M3_BOOTBIN_SHA=$(m3_bootbin_sha)\nm3_freeze\nm3_bootbin_report').stdout
        self.assertIn("this script froze update-m1n1", out)

    def test_report_catches_a_change(self):
        proc = self.run_sh(
            f"M3_BOOTBIN_SHA=$(m3_bootbin_sha)\nm3_freeze\nprintf x >>'{self.boot}'\nm3_bootbin_report",
            check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("boot.bin changed", proc.stderr)

    def test_m1n1_update_says_when_frozen(self):
        # m1n1_update on a frozen non-M3 Mac must not claim a rebuild.
        self.conf.write_text("M1N1_UPDATE_DISABLED=1\n")
        state = self.tmp / "state"
        state.mkdir()
        proc = self.run_sh(
            f"STATE='{state}'\n"
            # m1n1_update writes /etc/default/update-m1n1 itself; point it here.
            f"eval \"$(declare -f m1n1_update | command sed 's|conf=/etc/default/update-m1n1|conf={self.conf}|')\"\n"
            "m1n1_update", check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("was not rebuilt", proc.stderr)
        self.assertNotIn("Rebuilding m1n1", proc.stdout)


if __name__ == "__main__":
    unittest.main()
