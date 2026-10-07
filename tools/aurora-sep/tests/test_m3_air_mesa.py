"""mesa-m3 on the M3 MacBook Air (12.4 on), against the fake Mac of test_m3_flow.

Every Air (M3_AIR_BOARDS) gets the release's mesa-m3, the M3 Pro's asset, with the desktop user
added to group render, recorded and undone as on the M3 Pro. Its login hook leaves the Air's GPU
off ("experimental") until --m3-gpu-experiment writes the opt-in file (test_air_gpu covers the
file itself). The experiment's earlier mesa-m3-g15g is removed before mesa-m3's pacman -U, and
recorded. The M3 Pro runs as on 12.3, but for the recovery text's reason names.
"""
import difflib
import re
import subprocess
import unittest

import test_m3_flow as flow
import test_m3_pro_mesa as pro
import test_m3_air_default as air_default

SRC = flow.SRC
PRO_MESA_VERSION = flow.PRO_MESA_VERSION
REL_12_3 = air_default.REL_12_3
# 12.3's experiment Mesa, by the name its script ships.
G15G = "mesa-m3-g15g-26.1.4.g15g1-5-aarch64.pkg.tar.zst"
G15G_VERSION = "26.1.4.g15g1-5"


class AirMesaTest(flow.M3FlowBase):

    def setUp(self):
        super().setUp()
        self.optin = self.tmp / "etc/mesa-m3/t8122-gpu-experiment"
        self.bin = self.tmp / "usr-local-bin"
        (self.tmp / "bin/logger").write_text("#!/bin/sh\nexit 0\n")
        (self.tmp / "bin/logger").chmod(0o755)
        for name in re.findall(r'^  "(air-gpu-[a-z]+\.sh) [0-9a-f]{64}"$', SRC, re.M):
            (self.tmp / "pkgs" / name).write_bytes((flow.INSTALLER.parent / "air-gpu" / name).read_bytes())

    def sh(self, body, check=True, flag=0, installer=None):
        stub = 'm3_gpu_oneshot() { echo "air-gpu-oneshot.sh $*" >>"$FAKE/log"; return 0; }\n'
        if installer:
            self.installer = installer
        try:
            return self.run_sh(f"M3_GPU_BIN='{self.bin}'\nM3_GPU_EXPERIMENT={flag}\nM3_GPU_OPTIN='{self.optin}'\n"
                               f"{stub}{body}", check=check)
        finally:
            self.installer = flow.INSTALLER

    def record(self):
        return pro.parse_record(self, (self.state / "m3-pro-mesa").read_bytes())

    def installed(self):
        return (self.fake / "installed").read_text().split()

    def transactions(self):
        return [l for l in self.log().splitlines() if l.startswith("pacman -U ")]

    def me(self):
        return subprocess.run(["id", "-un"], capture_output=True, text=True).stdout.strip()

    def test_every_air_gets_mesa_m3(self):
        # The J613 with m1n1's display handoff (by default), the J615 kernel-only: the same Mesa.
        for board in ("j613", "j615"):
            with self.subTest(board=board):
                pro.reset_mac(self, board)
                proc = self.sh("M3_TRY=0\ninstall_all")
                self.assertIn(flow.PRO_MESA, self.downloaded())
                kernel, mesa = self.transactions()
                self.assertNotIn("mesa-m3", kernel)
                self.assertRegex(mesa, rf"^pacman -U --noconfirm \S+/m3-pro/{re.escape(flow.PRO_MESA)}$")
                rec, lists = self.record()
                self.assertEqual((rec["board"], rec["result"], rec["installed_by"], rec["installed_version"]),
                                 (f"apple,{board}", "installed", "installer", PRO_MESA_VERSION))
                self.assertEqual((rec["render_added"], rec["render_by_installer"]), ("yes", "yes"))
                self.assertEqual(lists["integration_path"], pro.INTEGRATION)
                self.assertEqual(lists["replaced_package"], [])
                out = " ".join(proc.stdout.split())
                self.assertIn(f"The M3 MacBook Air's Mesa (mesa-m3 {PRO_MESA_VERSION}, in /opt/mesa-m3) is installed.",
                              out)
                self.assertIn(f"Added {self.me()} to the render group, which the M3 MacBook Air's GPU now needs.", out)
                self.assertIn("This Air's GPU is experimental: GPU sessions need --m3-gpu-experiment (it writes "
                              f"{self.optin}). Until then each login says experimental and renders in software.", out)
                self.assertIn("Reasons: active, opt-out, not-supported, experimental, no-gpu,", out)
                self.assertNotIn("M3 Pro", out)
                # No opt-in and no experiment without the flag.
                self.assertFalse(self.optin.exists())
                self.assertFalse(self.bin.exists())
                self.assertFalse((self.state / "m3-gpu-experiment").exists())
                self.assertEqual(proc.returncode, 0)

    def test_with_the_experiment_the_summary_names_the_opt_in(self):
        self.mac("j613")
        proc = self.sh("M3_TRY=0\ninstall_all", flag=1)
        self.assertTrue(self.optin.exists())
        out = " ".join(proc.stdout.split())
        self.assertIn(f"{self.optin} is present: a login uses this Air's GPU, which is experimental, when the GPU "
                      "is started (sudo air-gpu-oneshot.sh start arms one boot); otherwise it renders in software.", out)
        self.assertIn("The Mesa prefix for air-gpu-job.sh: /opt/mesa-m3 (mesa-m3)", out)
        self.assertEqual(self.record()[0]["result"], "installed")

    def test_no_m3_mesa_on_an_air(self):
        self.mac("j615")
        with open(self.fake / "installed", "a") as f:
            f.write("mesa-m3-g15g\n")
        proc = self.sh("M3_PRO_MESA=0\nM3_TRY=0\ninstall_all")
        self.assertNotIn(flow.PRO_MESA, self.downloaded())
        self.assertFalse([l for l in self.log().splitlines() if l.startswith(("pacman -R", "gpasswd"))])
        self.assertIn("mesa-m3-g15g", self.installed())          # left as it is
        self.assertEqual(self.record()[0]["result"], "skipped-flag")
        self.assertIn("--no-m3-mesa: leaving out the M3 MacBook Air's Mesa (mesa-m3)", proc.stdout)

    def test_uninstall_on_an_air(self):
        self.mac("j613")
        self.sh("M3_TRY=0\ninstall_all")
        since = len(self.log())
        proc = self.sh("uninstall_all")
        log = self.log()[since:].splitlines()
        self.assertIn("pacman -Rn --noconfirm mesa-m3", log)
        self.assertIn(f"gpasswd -d {self.me()} render", log)
        self.assertLess(log.index("pacman -Rn --noconfirm mesa-m3"), log.index(f"gpasswd -d {self.me()} render"))
        self.assertNotIn("mesa-m3", self.installed())
        self.assertIn(f"Removed the M3 MacBook Air's Mesa (mesa-m3 {PRO_MESA_VERSION})", proc.stdout)
        self.assertFalse(self.state.exists())

    def test_an_air_without_a_record_runs_uninstall_as_before(self):
        # An Air this script never gave mesa-m3 (a mesa-m3 of its owner's): --uninstall leaves it
        # and does not even look, as 12.3 did.
        self.mac("j615")
        self.sh("M3_PRO_MESA=0\nM3_TRY=0\ninstall_all")
        (self.state / "m3-pro-mesa").unlink()
        with open(self.fake / "installed", "a") as f:
            f.write("mesa-m3\n")
        since = len(self.log())
        self.sh("uninstall_all")
        self.assertNotIn("mesa-m3", self.log()[since:])
        self.assertIn("mesa-m3", self.installed())

    # mesa-m3-g15g, the experiment's Mesa of 12.2 and 12.3.

    def g15g_fixture(self):
        root = self.tmp / "root-g15g"
        (root / "opt/mesa-m3-g15g/lib").mkdir(parents=True, exist_ok=True)
        (root / ".PKGINFO").write_text(f"pkgname = mesa-m3-g15g\npkgver = {G15G_VERSION}\narch = aarch64\n")
        path = self.tmp / "pkgs" / G15G
        subprocess.run(["bsdtar", "--zstd", "-cf", str(path), "-C", str(root), ".PKGINFO", "opt"], check=True)
        import hashlib
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def scripts_of(self, rev):
        # The experiment's scripts as release rev shipped them (their sha256 are its own).
        for name in re.findall(r'^  "(air-gpu-[a-z]+\.sh) [0-9a-f]{64}"$', SRC, re.M):
            data = subprocess.run(["git", "show", f"{rev}:tools/aurora-sep/air-gpu/{name}"],
                                  cwd=flow.INSTALLER.parent, capture_output=True, check=True).stdout if rev else \
                (flow.INSTALLER.parent / "air-gpu" / name).read_bytes()
            (self.tmp / "pkgs" / name).write_bytes(data)

    def from_12_3_with_the_experiment(self, board):
        old, _ = pro.old_installer(self, REL_12_3, "install-12.3.sh")
        sha = self.g15g_fixture()
        self.mac(board)
        self.scripts_of(REL_12_3)
        try:
            self.sh(f'M3_GPU_MESA_PACKAGE="{G15G} {sha}"\nM3_TRY=0\ninstall_all', flag=1, installer=old)
        finally:
            self.scripts_of(None)
        self.assertIn("mesa-m3-g15g", self.installed())
        self.assertIn("mesa mesa-m3-g15g", (self.state / "m3-gpu-experiment").read_text())
        (self.fake / "log").write_text("")

    def test_an_air_moves_from_mesa_m3_g15g(self):
        for board, flag in (("j615", 0), ("j613", 1)):
            with self.subTest(board=board, flag=flag):
                pro.reset_mac(self, board)
                self.from_12_3_with_the_experiment(board)
                proc = self.sh("M3_TRY=0\ninstall_all", flag=flag)
                log = self.log().splitlines()
                removal = "pacman -Rns --noconfirm mesa-m3-g15g"
                self.assertIn(removal, log)
                mesa = [l for l in log if l.startswith("pacman -U") and "/m3-pro/" in l]
                self.assertEqual(len(mesa), 1)
                # After the kernel, and right before mesa-m3's own transaction.
                self.assertLess(log.index(removal), log.index(mesa[0]))
                self.assertGreater(log.index(removal), log.index(self.transactions()[0]))
                self.assertNotIn("mesa-m3-g15g", self.installed())
                self.assertIn("mesa-m3", self.installed())
                rec, lists = self.record()
                self.assertEqual(lists["replaced_package"], [f"mesa-m3-g15g {G15G_VERSION}"])
                self.assertEqual(rec["result"], "installed")
                out = " ".join(proc.stdout.split())
                self.assertIn(f"Removed mesa-m3-g15g {G15G_VERSION}, which mesa-m3 replaces.", out)
                self.assertIn("Removing mesa-m3-g15g", out)
                self.assertNotIn(G15G, self.downloaded())
                # A rerun keeps it recorded, and removes nothing more.
                since = len(self.log())
                self.sh("M3_TRY=0\ninstall_all", flag=flag)
                self.assertNotIn("pacman -R", self.log()[since:])
                self.assertEqual(self.record()[1]["replaced_package"], [f"mesa-m3-g15g {G15G_VERSION}"])
                # --uninstall: mesa-m3 goes (this script's), the experiment's scripts too, and no
                # complaint about the mesa-m3-g15g its record still names.
                proc = self.sh("uninstall_all")
                self.assertNotIn("could not remove", proc.stderr)
                self.assertNotIn("mesa-m3", self.installed())
                self.assertNotIn("mesa-m3-g15g", self.installed())
                self.assertFalse(self.optin.exists())

    def test_a_g15g_that_will_not_go_leaves_mesa_m3_out(self):
        self.mac("j615")
        self.from_12_3_with_the_experiment("j615")
        body = 'pacman() { [[ $1 == -Rns && $3 == mesa-m3-g15g ]] && return 1; command pacman "$@"; }\n'
        proc = self.sh(body + "M3_TRY=0\ninstall_all", check=False)
        self.assertEqual(proc.returncode, 3)
        self.assertFalse([l for l in self.log().splitlines() if l.startswith("pacman -U") and "/m3-pro/" in l])
        self.assertIn("mesa-m3-g15g", self.installed())
        self.assertIn("linux-aurora", self.installed())
        err = " ".join(proc.stderr.split())
        self.assertIn("could not remove mesa-m3-g15g, which mesa-m3 replaces; so the M3 MacBook Air's Mesa was left "
                      "out.", err)
        self.assertIn("Remove it (sudo pacman -Rns mesa-m3-g15g), then run this again.", err)
        rec, lists = self.record()
        self.assertEqual((rec["result"], lists["replaced_package"]), ("failed", []))

    def test_the_m3_pro_does_not_remove_g15g(self):
        # Only an Air ever had the experiment's Mesa from this script; the M3 Pro runs as on 12.3.
        self.mac("j516s")
        with open(self.fake / "installed", "a") as f:
            f.write("mesa-m3-g15g\n")
        self.sh("M3_TRY=0\ninstall_all")
        self.assertNotIn("pacman -Rns --noconfirm mesa-m3-g15g", self.log())
        self.assertEqual(self.record()[1]["replaced_package"], [])


class M3ProAs123Test(flow.M3FlowBase):
    """The M3 Pro with 12.3's script and with this one: the same commands and files (test_m3_air_
    default.SameAs123Test), and an install's output differs in the recovery text's reason names
    only. The package version is the fake release's in both, as the release cut's fill sets it."""

    RUN = re.compile(r"run_id=[0-9a-f-]{36}")

    def test_only_the_reason_names_differ(self):
        old, old_version = pro.old_installer(self, REL_12_3, "install-12.3.sh")
        for board, try_ in (("j516s", 0), ("j514s", 0), ("j514s", 1)):
            with self.subTest(board=board, try_=try_):
                outs = []
                for installer in (old, flow.INSTALLER):
                    pro.reset_mac(self, board)
                    self.installer = installer
                    try:
                        proc = self.run_sh(f"M3_TRY={try_}\ninstall_all")
                    finally:
                        self.installer = flow.INSTALLER
                    out = self.RUN.sub("run_id=<id>", pro.MKTEMP.sub("<tmp>", proc.stdout))
                    # 12.3's release number read as this one's (its packages, tag and boot.bin copy).
                    outs.append(out.replace(old_version, flow.VERSION).splitlines())
                changed = [l for l in difflib.unified_diff(outs[0], outs[1], lineterm="", n=0)
                           if l[:1] in "+-" and not l.startswith(("+++", "---"))]
                self.assertEqual(changed, ["-" + l for l in air_default.REASONS_123.splitlines()]
                                 + ["+" + l for l in air_default.REASONS_124.splitlines()])


if __name__ == "__main__":
    unittest.main()
