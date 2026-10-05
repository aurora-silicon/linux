"""The installer's M3 handoff path, exercised on fixture files instead of a Mac.

Each case sources install-aurora-sep.sh for its functions only
(AURORA_SEP_SOURCE_ONLY=1), points the paths it touches at a temporary
directory, and replaces sudo and the EFI-partition lookup.
"""
from pathlib import Path
import gzip
import os
import shutil
import subprocess
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parent.parent / "install-aurora-sep.sh"

OUR_FREEZE = (
    "# >>> aurora-sep: keep this M3's boot.bin as it is (remove with: install-aurora-sep.sh --uninstall)\n"
    "M1N1_UPDATE_DISABLED=1\n"
    "# <<< aurora-sep: keep this M3's boot.bin as it is\n"
)
FREEZE = (
    "# Added by m3-gpu-work/scripts/install-m3gpu.sh: keep the M3 GPU candidate boot.bin.\n"
    "# Undo with rollback-m3gpu.sh (then run: sudo update-m1n1).\n"
    "M1N1_UPDATE_DISABLED=1\n"
)
SWITCHES = [
    "chosen.asahi,t6030-gpu=1",
    "chosen.asahi,t6030-dcp=1",
    "chosen.asahi,t6030-dcpext=1",
]
BOARDS = {
    "j516s": ["apple,j516s", "apple,t6030", "apple,arm-platform"],
    "j514s": ["apple,j514s", "apple,t6030", "apple,arm-platform"],
    "j514c": ["apple,j514c", "apple,t6031", "apple,arm-platform"],
    "j293": ["apple,j293", "apple,t8103", "apple,arm-platform"],
    "j613": ["apple,j613", "apple,t8122", "apple,arm-platform"],
    "j314s": ["apple,j314s", "apple,t6000", "apple,arm-platform"],
    "j700": ["apple,j700", "apple,t8140", "apple,arm-platform"],
}
GOOD_IBOOT = "iBoot-10151.140.19.700.2"


class M3PathTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.dt = self.tmp / "dt"
        (self.dt / "chosen").mkdir(parents=True)
        self.etc = self.tmp / "etc"
        (self.etc / "default").mkdir(parents=True)
        self.state = self.tmp / "state"
        self.state.mkdir()
        self.esp = self.tmp / "esp"
        (self.esp / "m1n1").mkdir(parents=True)
        (self.esp / "asahi").mkdir()
        (self.esp / "m1n1" / "boot.bin").write_bytes(b"m1n1-old\x00\x01" * 8)

    def mac(self, board, iboot=GOOD_IBOOT, stub="14.8.3"):
        (self.dt / "compatible").write_bytes(
            b"".join(c.encode() + b"\0" for c in BOARDS[board])
        )
        if iboot is not None:
            (self.dt / "chosen" / "asahi,iboot2-version").write_bytes(iboot.encode() + b"\0")
        info = self.esp / "asahi" / "stub_info.json"
        if stub is None:
            info.unlink(missing_ok=True)
        else:
            info.write_text(
                '{"vgid": "x", "system_version": {"ProductUserVisibleVersion": "%s (stub)", '
                '"ProductVersion": "%s", "ProductBuildVersion": "23J220"}}' % (stub, stub)
            )

    def run_sh(self, body, check=True):
        script = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
sudo=""
DT='{self.dt}'
STATE='{self.state}'
M1N1_CONF='{self.etc}/m1n1.conf'
UPDATE_M1N1_CONF='{self.etc}/default/update-m1n1'
M3GPU_MARKER='{self.etc}/default/.update-m1n1.created-by-m3gpu'
esp_bootbin() {{ [[ -f '{self.esp}/m1n1/boot.bin' ]] && echo '{self.esp}/m1n1/boot.bin'; }}
{body}
"""
        proc = subprocess.run(
            ["bash", "-c", script], capture_output=True, text=True,
            env={**os.environ, "NO_COLOR": "1"},
        )
        if check:
            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        return proc

    # Boards

    def test_board_classes(self):
        expect = {"j516s": "m3 handoff", "j514s": "m3 off", "j613": "m3 off",
                  "j514c": "m3 off", "j314s": "not-m3", "j700": "not-m3 neo"}
        for board, want in expect.items():
            with self.subTest(board=board):
                self.mac(board)
                out = self.run_sh(
                    'if is_m3; then printf m3; else printf not-m3; fi\n'
                    'if is_m3; then if is_m3_handoff_board; then printf " handoff"; else printf " off"; fi; fi\n'
                    'if is_neo; then printf " neo"; fi\n'
                ).stdout
                self.assertEqual(out, want)

    def test_board_list_is_one_line(self):
        self.mac("j514s")
        out = self.run_sh('M3_HANDOFF_BOARDS="j516s j514s"\nis_m3_handoff_board && echo yes').stdout
        self.assertEqual(out.strip(), "yes")

    def test_board_list_never_takes_a_non_m3_pro(self):
        # The switches are T6030 ones: listing an M3 or M3 Max board must not
        # give it the handoff.
        self.mac("j514c")
        out = self.run_sh('M3_HANDOFF_BOARDS="j516s j514c"\nis_m3_handoff_board && echo yes || echo no').stdout
        self.assertEqual(out.strip(), "no")

    # Stub

    def test_stub_ok(self):
        self.mac("j516s")
        self.assertEqual(self.run_sh("m3_stub_problem").stdout, "")

    def test_stub_other_version(self):
        self.mac("j516s", stub="15.6")
        self.assertIn("stub is 15.6", self.run_sh("m3_stub_problem").stdout)

    def test_stub_info_missing_falls_back_to_iboot(self):
        self.mac("j516s", stub=None)
        self.assertEqual(self.run_sh("m3_stub_problem").stdout, "")
        self.mac("j516s", stub=None, iboot="iBoot-11881.1.1")
        self.assertIn("iBoot-11881.1.1", self.run_sh("m3_stub_problem").stdout)

    def test_stub_iboot_unreported(self):
        self.mac("j516s", iboot=None, stub=None)
        self.assertIn("not reported", self.run_sh("m3_stub_problem").stdout)

    # Plan

    def plan(self, try_=0):
        return self.run_sh(f"M3_TRY={try_}\nm3_plan >/dev/null 2>&1\necho $M3_MODE").stdout.strip()

    def test_plan_modes(self):
        for board, stub, want in [("j516s", "14.8.3", "handoff"), ("j516s", "15.6", "kernel"),
                                  ("j514s", "14.8.3", "kernel"), ("j613", "14.8.3", "kernel"),
                                  ("j514c", "14.8.3", "kernel"),
                                  ("j314s", "13.5", "none"), ("j700", "26.4", "none")]:
            with self.subTest(board=board, stub=stub):
                self.mac(board, stub=stub)
                self.assertEqual(self.plan(), want)

    def test_plan_try_on_an_unlisted_m3_pro(self):
        self.mac("j514s")
        self.assertEqual(self.plan(try_=1), "handoff")
        out = self.run_sh("M3_TRY=1\nm3_plan").stderr
        self.assertIn("nobody", out)

    def test_plan_try_refused_elsewhere(self):
        for board, stub, why in [("j613", "14.8.3", "M3 Pro"), ("j514c", "14.8.3", "M3 Pro"),
                                 ("j314s", "13.5", "isn't an M3"), ("j700", "26.4", "isn't an M3"),
                                 ("j514s", "15.6", "stub is 15.6"), ("j516s", "15.6", "stub is 15.6")]:
            with self.subTest(board=board, stub=stub):
                self.mac(board, stub=stub)
                proc = self.run_sh("M3_TRY=1\nm3_plan", check=False)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(why, proc.stderr)
                self.assertIn("Nothing was installed", proc.stderr)

    def test_plan_handoff_over_an_11_36_kernel_only_install(self):
        self.mac("j516s")
        (self.etc / "default" / "update-m1n1").write_text(OUR_FREEZE)
        self.assertEqual(self.plan(), "handoff")

    def test_plan_handoff_with_bringup_freeze(self):
        self.mac("j516s")
        (self.etc / "default" / "update-m1n1").write_text(FREEZE)
        self.assertEqual(self.plan(), "handoff")

    def test_plan_refuses_someone_elses_freeze(self):
        self.mac("j516s")
        (self.etc / "default" / "update-m1n1").write_text("M1N1_UPDATE_DISABLED=1\n")
        proc = self.run_sh("m3_plan", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)

    # Bring-up freeze

    def test_unfreeze_file_the_bringup_created(self):
        conf = self.etc / "default" / "update-m1n1"
        marker = self.etc / "default" / ".update-m1n1.created-by-m3gpu"
        conf.write_text(FREEZE)
        marker.touch()
        self.run_sh("m3gpu_unfreeze")
        self.assertFalse(conf.exists())
        self.assertFalse(marker.exists())
        self.assertEqual((self.state / "update-m1n1.m3gpu.saved").read_text(), FREEZE)
        self.assertTrue((self.state / "m3gpu-marker.saved").exists())

    def test_unfreeze_keeps_the_rest_of_the_file(self):
        conf = self.etc / "default" / "update-m1n1"
        conf.write_text("M1N1=/usr/lib/neo/m1n1.bin\n" + FREEZE + "U_BOOT=/x\n")
        self.run_sh("m3gpu_unfreeze")
        self.assertEqual(conf.read_text(), "M1N1=/usr/lib/neo/m1n1.bin\nU_BOOT=/x\n")

    def test_unfreeze_leaves_other_freezes(self):
        conf = self.etc / "default" / "update-m1n1"
        conf.write_text("M1N1_UPDATE_DISABLED=1\n")
        self.run_sh("m3gpu_unfreeze")
        self.assertEqual(conf.read_text(), "M1N1_UPDATE_DISABLED=1\n")
        self.assertFalse((self.state / "update-m1n1.m3gpu.saved").exists())

    def test_frozen_by_others(self):
        conf = self.etc / "default" / "update-m1n1"
        for text, want in [(FREEZE, "no"), (OUR_FREEZE, "no"), (FREEZE + OUR_FREEZE, "no"),
                           ("M1N1_UPDATE_DISABLED=1\n", "yes"),
                           ("# M1N1_UPDATE_DISABLED=1\n", "no"), (FREEZE + "export M1N1_UPDATE_DISABLED=y\n", "yes")]:
            with self.subTest(text=text):
                conf.write_text(text)
                out = self.run_sh("update_m1n1_frozen_by_others && echo yes || echo no").stdout.strip()
                self.assertEqual(out, want)

    # /etc/m1n1.conf

    def test_switches_write_and_remove(self):
        conf = self.etc / "m1n1.conf"
        conf.write_text("display=2560x1600\n")
        self.run_sh("m3_switches_write\nm3_switches_write")
        lines = conf.read_text().splitlines()
        self.assertEqual(lines[0], "display=2560x1600")
        self.assertEqual([l for l in lines if l.startswith("chosen.")], SWITCHES)
        self.run_sh("m3_switches_remove")
        self.assertEqual(conf.read_text(), "display=2560x1600\n")

    def test_switches_remove_deletes_a_file_of_only_ours(self):
        conf = self.etc / "m1n1.conf"
        self.run_sh("m3_switches_write")
        self.assertTrue(conf.exists())
        self.run_sh("m3_switches_remove")
        self.assertFalse(conf.exists())

    def test_switches_remove_leaves_hand_written_lines(self):
        conf = self.etc / "m1n1.conf"
        conf.write_text("chosen.asahi,t6030-gpu=1\n")
        self.run_sh("m3_switches_remove")
        self.assertEqual(conf.read_text(), "chosen.asahi,t6030-gpu=1\n")

    # Package and boot.bin checks

    def package(self, strings):
        if not shutil.which("bsdtar") or not shutil.which("zstd"):
            self.skipTest("bsdtar and zstd are needed to build a fixture package")
        root = self.tmp / "pkgroot"
        (root / "usr/lib/asahi-boot").mkdir(parents=True)
        (root / "usr/lib/asahi-boot/m1n1.bin").write_bytes(
            b"\x7fm1n1\x00" + b"\x00".join(s.encode() for s in strings) + b"\x00tail"
        )
        pkg = self.tmp / "m1n1-aurora-test-1-aarch64.pkg.tar.zst"
        subprocess.run(["bsdtar", "--zstd", "-cf", str(pkg), "-C", str(root), "usr"], check=True)
        return pkg

    def test_package_with_handoff(self):
        pkg = self.package(["asahi,t6030-gpu", "asahi,t6030-dcp", "asahi,t6030-dcpext"])
        self.assertEqual(self.run_sh(f"m1n1_pkg_has_handoff '{pkg}' && echo yes").stdout.strip(), "yes")

    def test_package_without_dcpext(self):
        pkg = self.package(["asahi,t6030-gpu", "asahi,t6030-dcp"])
        self.assertEqual(self.run_sh(f"m1n1_pkg_has_handoff '{pkg}' && echo yes || echo no").stdout.strip(), "no")

    def test_verify_bootbin(self):
        # As update-m1n1 writes it: the first config line follows the gzip
        # stream directly, with no newline in front.
        boot = self.esp / "m1n1" / "boot.bin"
        boot.write_bytes(b"m1n1" + gzip.compress(b"u-boot" * 64) + "\n".join(SWITCHES).encode() + b"\n")
        self.run_sh("m3_verify_bootbin")
        boot.write_bytes(b"m1n1" + gzip.compress(b"u-boot" * 64) + b"display=2560x1600\n"
                         + "\n".join(SWITCHES).encode() + b"\n")
        self.run_sh("m3_verify_bootbin")
        boot.write_bytes(b"m1n1" + gzip.compress(b"u-boot" * 64) + b"chosen.asahi,t6030-gpu=1\n")
        proc = self.run_sh("m3_verify_bootbin", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("switch lines", proc.stderr)

    def test_verify_real_layout(self):
        # The m3pro's hand-built boot.bin ends exactly like this (2026-10-03).
        boot = self.esp / "m1n1" / "boot.bin"
        boot.write_bytes(b"\x1f\x8b" + os.urandom(900) + b"chosen.asahi,t6030-gpu=1\n"
                         b"chosen.asahi,t6030-dcp=1\nchosen.asahi,t6030-dcpext=1\n")
        self.run_sh("m3_verify_bootbin")

    # Which packages each Mac gets

    def packages(self, board, stub="14.8.3", try_=0):
        self.mac(board, stub=stub)
        out = self.run_sh(f"M3_TRY={try_}\nm3_plan >/dev/null 2>&1\npackages_for_this_mac").stdout
        return [line.split()[0] for line in out.splitlines()]

    def test_packages_per_mac(self):
        m1n1_m1m2 = "m1n1-aurora-1.6.1.aurora3-1-aarch64.pkg.tar.zst"
        m1n1_m3 = "m1n1-aurora-1.6.1.aurora5-1-aarch64.pkg.tar.zst"
        for board, stub, try_, want in [("j314s", "13.5", 0, m1n1_m1m2), ("j293", "13.5", 0, m1n1_m1m2),
                                        ("j700", "26.4", 0, None), ("j613", "14.8.3", 0, None),
                                        ("j514c", "14.8.3", 0, None), ("j514s", "14.8.3", 0, None),
                                        ("j516s", "15.6", 0, None), ("j516s", "14.8.3", 0, m1n1_m3),
                                        ("j514s", "14.8.3", 1, m1n1_m3)]:
            with self.subTest(board=board, stub=stub, try_=try_):
                got = self.packages(board, stub, try_)
                m1n1 = [p for p in got if p.startswith("m1n1-aurora-")]
                self.assertEqual(m1n1, [want] if want else [])
                self.assertIn("linux-aurora-7.1.12.aurora2-11.36-aarch64.pkg.tar.zst", got)
                self.assertEqual(len(got), 5 if want else 4)

    # Uninstall

    def test_restore_bringup(self):
        conf = self.etc / "default" / "update-m1n1"
        marker = self.etc / "default" / ".update-m1n1.created-by-m3gpu"
        conf.write_text(FREEZE)
        marker.touch()
        handbuilt = (self.esp / "m1n1" / "boot.bin").read_bytes()
        self.run_sh("m3gpu_unfreeze")
        (self.state / "boot.bin.saved").write_bytes(handbuilt)
        (self.esp / "m1n1" / "boot.bin").write_bytes(b"stock rebuild")
        self.run_sh("m3_restore_bringup")
        self.assertEqual(conf.read_text(), FREEZE)
        self.assertTrue(marker.exists())
        self.assertEqual((self.esp / "m1n1" / "boot.bin").read_bytes(), handbuilt)

    def test_restore_bringup_noop_elsewhere(self):
        self.run_sh("m3_restore_bringup")
        self.assertFalse((self.etc / "default" / "update-m1n1").exists())


if __name__ == "__main__":
    unittest.main()
