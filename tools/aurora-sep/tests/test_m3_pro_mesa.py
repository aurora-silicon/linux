"""The M3 Pro's Mesa (mesa-m3), on by default on every M3 Pro (t6030), against the fake Mac of
test_m3_flow.

ProMesaTest: an M3 Pro downloads the package with the other release assets, installs it in a
pacman transaction of its own after the kernel, and records what happened in $STATE/m3-pro-mesa;
--no-m3-mesa, dependencies that are too old, a pacman failure, reruns, upgrades, --uninstall, and a
setup of the owner's own that is left alone.

OtherMacsTest: every other Mac (M1, M2, the M3 Max, the M3 MacBook Air) runs exactly the commands
12.2's script ran, with the same results on disk: nothing Mesa is downloaded, installed, recorded
or removed, and a Mesa the owner installed stays. Every $sudo command is logged too, and the
logs are compared line by line. The Air with --m3-gpu-experiment is compared in test_air_gpu.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import test_m3_flow as flow

SRC = flow.SRC
VERSION = flow.VERSION
PRO_MESA = flow.PRO_MESA
PRO_MESA_VERSION = flow.PRO_MESA_VERSION
# 12.2's script, the last one without the M3 Pro's Mesa.
REL_12_2 = "26cdc069"
# Every $sudo command goes to the fake's log as well, then runs as before.
SUDO_LOG = 'fake_sudo() { echo "sudo $*" >>"$FAKE/log"; "$@"; }\nsudo=fake_sudo\n'
# What mktemp makes: another name on every run.
MKTEMP = re.compile(r"(?:/tmp|" + re.escape(tempfile.gettempdir()) + r")/tmp\.[A-Za-z0-9]{6,}")
# A session setup of the owner's own, as an M3 Pro owner might have it before 12.3.
CHONKSTEP_ENV = (b"CHONKSTEP_M3_CLIENTS=gpu\n"
                 b"CHONKSTEP_M3_MESA_PREFIX=/home/owner/src/mesa-prefix\n"
                 b"CHONKSTEP_M3_XWAYLAND_GLAMOR=1\n")


def old_installer(tc, rev, name):
    old = subprocess.run(["git", "show", f"{rev}:tools/aurora-sep/install-aurora-sep.sh"],
                         cwd=flow.INSTALLER.parent, capture_output=True)
    if old.returncode:
        tc.skipTest(f"git can't show the earlier script ({rev})")
    path = tc.tmp / name
    path.write_bytes(old.stdout)
    version = re.search(rb"^VERSION=(\S+)$", old.stdout, re.M).group(1).decode()
    return path, version


def reset_mac(tc, board):
    # Everything a run can write goes; the packages, stubs and scripts stay.
    keep = {"pkgs", "bin"}
    for p in tc.tmp.iterdir():
        if p.name in keep or p.name.startswith(("root-", "install-")):
            continue
        shutil.rmtree(p) if p.is_dir() and not p.is_symlink() else p.unlink()
    for d in ("dt/chosen", "esp/m1n1", "esp/asahi", "etc/default", "state", "fake", "home"):
        (tc.tmp / d).mkdir(parents=True, exist_ok=True)
    tc.mac(board)


def tree(tc):
    # Every file a run can touch, by path, with its bytes.
    skip = ("pkgs/", "bin/", "fake/log")
    out = {}
    for p in sorted(tc.tmp.rglob("*")):
        rel = p.relative_to(tc.tmp).as_posix()
        if rel.startswith(skip) or rel.startswith(("root-", "install-")) or not p.is_file():
            continue
        out[rel] = p.read_bytes()
    return out


def run_with(tc, installer, board, run, setup=None):
    """Runs `run` with `installer` on a fresh fake `board` (after `setup`), and returns what is
    compared: the exit statuses, the command log and every file a run can touch."""
    reset_mac(tc, board)
    if setup:
        setup()
    tc.installer = installer
    try:
        codes = run()
    finally:
        tc.installer = flow.INSTALLER
    return {"codes": codes, "log": MKTEMP.sub("<tmp>", tc.log()), "tree": tree(tc)}


def owner_mesa(tc):
    # A Mac whose owner installed Mesa packages and a prefix of their own, and set up a session
    # for it, before this script ever ran.
    with open(tc.fake / "installed", "a") as f:
        f.write("mesa-m3\nmesa-m3-g15g\n")
    (tc.fake / "versions").write_text("mesa-m3 26.0.0.owner-1\nmesa-m3-g15g 26.1.4.g15g1-5\n")
    (tc.tmp / "opt/mesa-m3/lib").mkdir(parents=True)
    (tc.tmp / "opt/mesa-m3/lib/libvulkan_asahi.so").write_bytes(b"the owner's own build")
    (tc.tmp / "home/.config/chonkstep").mkdir(parents=True)
    (tc.tmp / "home/.config/chonkstep/m3gpu-session.env").write_bytes(CHONKSTEP_ENV)


class ProMesaTest(flow.M3FlowBase):

    def record(self):
        path = self.state / "m3-pro-mesa"
        return dict(l.split("=", 1) for l in path.read_text().splitlines())

    def installed(self):
        return (self.fake / "installed").read_text().split()

    def mesa_lines(self, since=0):
        # What changes packages (pacman -U, -R...), for the Mesa; not the -Q queries.
        return [l for l in self.log()[since:].splitlines()
                if l.startswith("pacman") and not l.startswith("pacman -Q") and "mesa" in l]

    def older(self, version="26.1.3.g15s1-1"):
        name = f"mesa-m3-{version}-aarch64.pkg.tar.zst"
        self.pro_mesa_fixture(name, "mesa-m3", version, "opt/mesa-m3")
        return name

    def owner_has(self, version):
        with open(self.fake / "installed", "a") as f:
            f.write("mesa-m3\n")
        (self.fake / "versions").write_text(f"mesa-m3 {version}\n")

    def test_every_m3_pro_gets_it_after_the_kernel(self):
        for board in ("j516s", "j514s"):
            with self.subTest(board=board):
                reset_mac(self, board)
                proc = self.install()
                self.assertIn(PRO_MESA, self.downloaded())
                lines = self.log().splitlines()
                u = [i for i, l in enumerate(lines) if l.startswith("pacman -U")]
                # Two transactions: the kernel's (without the Mesa package), then the Mesa's alone.
                self.assertEqual(len(u), 2, lines)
                self.assertNotIn("mesa", lines[u[0]])
                self.assertIn("linux-aurora-", lines[u[0]])
                self.assertRegex(lines[u[1]], rf"^pacman -U --noconfirm \S+/m3-pro/{re.escape(PRO_MESA)}$")
                self.assertIn("mesa-m3", self.installed())
                rec = self.record()
                self.assertEqual(rec["package"], "mesa-m3")
                self.assertEqual(rec["version"], PRO_MESA_VERSION)
                self.assertEqual(rec["file"], PRO_MESA)
                self.assertEqual(rec["sha256"], self.shas[PRO_MESA])
                self.assertEqual(rec["prefix"], "/opt/mesa-m3")
                self.assertEqual(rec["result"], "installed")
                self.assertEqual(rec["installed_version"], PRO_MESA_VERSION)
                self.assertEqual(rec["installed_by"], "installer")
                self.assertEqual(rec["preexisting"], "none")
                self.assertEqual(rec["user_setup"], "none")
                self.assertEqual(rec["opt_out"], "none")
                self.assertEqual(rec["created_files"], "0")
                out = " ".join(proc.stdout.split())
                self.assertIn(f"The M3 Pro's Mesa (mesa-m3 {PRO_MESA_VERSION}, in /opt/mesa-m3) is installed.", out)
                self.assertIn("The new graphics take effect at the next login", out)
                # j514s is not on the handoff list: kernel-only, so the package's login check stays off.
                self.assertEqual("boots kernel-only (no GPU handoff)" in out, board == "j514s")

    def test_the_invoking_user_is_recorded(self):
        self.mac("j516s")
        self.extra_env["SUDO_USER"] = "someone"
        self.install()
        self.assertEqual(self.record()["user"], "someone")
        self.extra_env.pop("SUDO_USER")
        self.extra_env["USER"] = "ignored"
        self.install()
        me = subprocess.run(["id", "-un"], capture_output=True, text=True).stdout.strip()
        self.assertEqual(self.record()["user"], me)

    def test_no_m3_mesa_leaves_it_out(self):
        self.mac("j516s")
        proc = self.install(env="M3_PRO_MESA=0")
        self.assertNotIn(PRO_MESA, self.downloaded())
        self.assertEqual(self.mesa_lines(), [])
        self.assertNotIn("mesa-m3", self.installed())
        self.assertEqual(self.record()["result"], "skipped-flag")
        self.assertIn("--no-m3-mesa: leaving out the M3 Pro's Mesa (mesa-m3)", proc.stdout)
        self.assertIn("the desktop renders in software", proc.stdout)
        self.assertIn("linux-aurora", self.installed())

    def test_no_m3_mesa_keeps_an_installed_copy(self):
        self.mac("j516s")
        self.install()
        since = len(self.log())
        proc = self.install(env="M3_PRO_MESA=0")
        self.assertEqual(self.mesa_lines(since), [])     # neither -U nor -R
        self.assertIn("mesa-m3", self.installed())
        self.assertIn(f"the {PRO_MESA_VERSION} installed earlier stays as it is", proc.stdout)
        rec = self.record()
        self.assertEqual(rec["result"], "skipped-flag")
        self.assertEqual(rec["preexisting"], "none")     # still this script's: --uninstall removes it

    def test_dependencies_too_old(self):
        self.mac("j516s")
        self.extra_env.update(FAKE_GLIBC="2.42+r1-1", FAKE_SPIRV_TOOLS="")
        proc = self.install(check=False)
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertIn("linux-aurora", self.installed())      # the kernel install is complete
        self.assertIn(PRO_MESA, self.downloaded())
        self.assertFalse([l for l in self.mesa_lines() if l.startswith("pacman -U")])
        self.assertNotIn("mesa-m3", self.installed())
        err = " ".join(proc.stderr.split())
        self.assertIn("glibc 2.42+r1-1 (needs 2.43 or newer); spirv-tools not installed (needs 1:1.4.357.0 or newer)", err)
        self.assertIn("sudo pacman -Syu glibc spirv-tools", err)
        self.assertEqual(err.count("left out the M3 Pro's Mesa"), 1)
        self.assertEqual(self.record()["result"], "skipped-deps")
        out = " ".join(proc.stdout.split())
        self.assertIn("The M3 Pro's Mesa was NOT installed: it needs newer packages", out)
        self.assertIn("The exit status is 3.", out)

    def test_a_bare_name_needs_only_to_be_installed(self):
        self.mac("j516s")
        self.extra_env.update(FAKE_SPIRV_TOOLS="")
        proc = self.install(env='M3_PRO_MESA_NEEDS="glibc>=2.43 spirv-tools"', check=False)
        self.assertEqual(proc.returncode, 3)
        self.assertIn("spirv-tools not installed.", " ".join(proc.stderr.split()))
        self.extra_env.update(FAKE_SPIRV_TOOLS="1:1.0-1")
        self.install(env='M3_PRO_MESA_NEEDS="glibc>=2.43 spirv-tools"')
        self.assertEqual(self.record()["result"], "installed")

    def test_a_pacman_failure_keeps_the_kernel_install(self):
        self.mac("j516s")
        self.extra_env["FAKE_FAIL_U_FOR"] = "mesa-m3-*"
        proc = self.install(check=False)
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertIn("linux-aurora", self.installed())
        self.assertNotIn("mesa-m3", self.installed())
        self.assertIn("could not install the M3 Pro's Mesa (mesa-m3)", " ".join(proc.stderr.split()))
        self.assertIn("pacman could not install it", " ".join(proc.stdout.split()))
        self.assertEqual(self.record()["result"], "failed")
        # The next run installs it.
        self.extra_env.pop("FAKE_FAIL_U_FOR")
        self.install()
        self.assertEqual(self.record()["result"], "installed")

    def test_a_package_by_another_name_is_not_installed(self):
        self.mac("j516s")
        self.pro_mesa_fixture(PRO_MESA, "mesa", PRO_MESA_VERSION, "usr")
        proc = self.install(check=False)
        self.assertEqual(proc.returncode, 3)
        self.assertFalse([l for l in self.mesa_lines() if l.startswith("pacman -U")])
        self.assertIn('names the package "mesa", not mesa-m3', " ".join(proc.stderr.split()))
        self.assertEqual(self.record()["result"], "failed")

    def test_a_failed_kernel_install_installs_no_mesa(self):
        self.mac("j516s")
        self.extra_env["FAKE_FAIL_U_FOR"] = "linux-aurora-*"
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertNotIn("mesa-m3", self.installed())
        self.assertFalse([l for l in self.mesa_lines() if l.startswith("pacman -U")])
        self.assertFalse((self.state / "m3-pro-mesa").exists())

    def test_same_version_rerun_installs_nothing_new(self):
        self.mac("j516s")
        self.install()
        since, n = len(self.log()), len(self.downloaded())
        proc = self.install()
        self.assertNotIn(PRO_MESA, self.downloaded()[n:])
        self.assertEqual(self.mesa_lines(since), [])
        self.assertIn(f"The M3 Pro's Mesa (mesa-m3 {PRO_MESA_VERSION}) is installed already", proc.stdout)
        rec = self.record()
        self.assertEqual(rec["result"], "current")
        self.assertEqual(rec["preexisting"], "none")

    def test_an_older_one_from_an_earlier_run_is_upgraded(self):
        self.mac("j516s")
        new = self.pro_mesa
        self.pro_mesa = self.older()
        self.install()
        self.assertEqual(self.record()["installed_version"], "26.1.3.g15s1-1")
        self.pro_mesa = new
        since = len(self.log())
        self.install()
        self.assertEqual(len([l for l in self.mesa_lines(since) if l.startswith("pacman -U")]), 1)
        rec = self.record()
        self.assertEqual((rec["result"], rec["installed_version"], rec["preexisting"]),
                         ("installed", PRO_MESA_VERSION, "none"))
        self.uninstall()
        self.assertNotIn("mesa-m3", self.installed())

    def test_a_newer_one_is_kept(self):
        self.mac("j516s")
        self.owner_has("27.0.0-1")
        proc = self.install()
        self.assertNotIn(PRO_MESA, self.downloaded())
        self.assertEqual(self.mesa_lines(), [])
        self.assertIn(f"A newer mesa-m3 (27.0.0-1) than this release's ({PRO_MESA_VERSION}) is installed; keeping it",
                      proc.stdout)
        self.assertEqual(self.record()["result"], "newer-kept")

    def test_uninstall_removes_what_this_script_installed(self):
        self.mac("j516s")
        self.install()
        since = len(self.log())
        proc = self.uninstall()
        self.assertIn("pacman -Rn --noconfirm mesa-m3", self.log()[since:].splitlines())
        self.assertNotIn("mesa-m3", self.installed())
        self.assertIn(f"Removed the M3 Pro's Mesa (mesa-m3 {PRO_MESA_VERSION})", proc.stdout)
        self.assertFalse((self.state / "m3-pro-mesa").exists())

    def test_uninstall_keeps_the_owners_own(self):
        # Installed before this script first ran, at this release's version, an older one this
        # script updated, or one it has no record of: it stays.
        for case in ("same", "older", "no-record"):
            with self.subTest(case=case):
                reset_mac(self, "j516s")
                self.owner_has(PRO_MESA_VERSION if case != "older" else "26.0.0-1")
                self.install()
                if case == "no-record":
                    (self.state / "m3-pro-mesa").unlink()
                since = len(self.log())
                proc = self.uninstall()
                self.assertEqual(self.mesa_lines(since), [])
                self.assertIn("mesa-m3", self.installed())
                self.assertIn("Keeping mesa-m3", proc.stdout)

    def test_uninstall_after_no_m3_mesa_removes_nothing(self):
        self.mac("j516s")
        self.install(env="M3_PRO_MESA=0")
        self.assertEqual(self.record()["installed_by"], "none")
        since = len(self.log())
        self.uninstall()
        self.assertEqual(self.mesa_lines(since), [])

    def test_one_the_owner_installed_after_this_script_stays(self):
        # --no-m3-mesa (or a failed install), then the owner installs mesa-m3 themselves.
        for first in ("M3_PRO_MESA=0", "FAIL"):
            with self.subTest(first=first):
                reset_mac(self, "j516s")
                if first == "FAIL":
                    self.extra_env["FAKE_FAIL_U_FOR"] = "mesa-m3-*"
                    self.install(check=False)
                    self.extra_env.pop("FAKE_FAIL_U_FOR")
                else:
                    self.install(env=first)
                self.owner_has(PRO_MESA_VERSION)
                self.install(env="M3_PRO_MESA=0")
                self.assertEqual(self.record()["installed_by"], "owner")
                since = len(self.log())
                proc = self.uninstall()
                self.assertEqual(self.mesa_lines(since), [])
                self.assertIn("Keeping mesa-m3", proc.stdout)
                self.assertIn("mesa-m3", self.installed())

    def test_a_rerun_keeps_it_this_scripts(self):
        # current, --no-m3-mesa and a failed upgrade keep "installer" while it stays installed.
        self.mac("j516s")
        self.install()
        self.install()
        self.assertEqual(self.record()["installed_by"], "installer")
        self.install(env="M3_PRO_MESA=0")
        self.assertEqual(self.record()["installed_by"], "installer")
        self.uninstall()
        self.assertNotIn("mesa-m3", self.installed())

    def test_a_failed_removal_is_a_warning(self):
        self.mac("j516s")
        self.install()
        proc = self.run_sh("pacman() { [[ $1 == -Rn ]] && return 1; command pacman \"$@\"; }\nuninstall_all")
        self.assertIn("could not remove mesa-m3; remove it with: sudo pacman -R mesa-m3", " ".join(proc.stderr.split()))
        self.assertIn("Done. Reboot to run", proc.stdout)

    def test_the_owners_setup_is_left_as_it_is(self):
        self.mac("j516s")
        home = self.tmp / "home"
        files = {
            ".config/chonkstep/m3gpu-session.env": CHONKSTEP_ENV,
            ".config/environment.d/90-vulkan.conf": b"VK_ICD_FILENAMES=/home/owner/icd.json\n",
            ".config/environment.d/91-ours.conf": b"VK_ICD_FILENAMES=/opt/mesa-m3/share/vulkan/icd.d/x.json\n",
            ".config/hypr/hyprland.conf": b"env = LIBGL_DRIVERS_PATH,/home/owner/dri\n",
            ".drirc": b"<driconf/>\n",
            ".config/mesa-m3/disable": b"",
            "unrelated.txt": b"keep me\n",
        }
        for rel, data in files.items():
            (home / rel).parent.mkdir(parents=True, exist_ok=True)
            (home / rel).write_bytes(data)
        before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in home.rglob("*") if p.is_file()}
        proc = self.install()
        self.assertEqual(self.record()["result"], "installed")       # still on by default
        out = " ".join(proc.stdout.split())
        self.assertIn(f"Left as it is: {self.record()['user']}'s own M3 Mesa setup ({home}/.config/chonkstep/"
                      "m3gpu-session.env: CHONKSTEP_M3_MESA_PREFIX=/home/owner/src/mesa-prefix;", out)
        for rel in (".config/environment.d/90-vulkan.conf", ".config/hypr/hyprland.conf", ".drirc"):
            self.assertIn(f"{home}/{rel}", out)
        self.assertNotIn("91-ours.conf", out)                          # it points at the package's prefix
        self.assertIn(f"switched off at login by {home}/.config/mesa-m3/disable", out)
        rec = self.record()
        self.assertEqual(rec["user_setup"].split(), [
            f"{home}/.config/chonkstep/m3gpu-session.env", f"{home}/.config/environment.d/90-vulkan.conf",
            f"{home}/.config/hypr/hyprland.conf", f"{home}/.drirc"])
        self.assertEqual(rec["opt_out"], f"{home}/.config/mesa-m3/disable")
        self.uninstall()
        after = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in home.rglob("*") if p.is_file()}
        self.assertEqual(after, before)

    def test_the_system_opt_out_is_reported_and_kept(self):
        self.mac("j516s")
        off = self.tmp / "etc/mesa-m3/disable"
        off.parent.mkdir(parents=True)
        off.write_text("")
        proc = self.install()
        self.assertIn(f"switched off at login by {off}: off for every user", " ".join(proc.stdout.split()))
        self.assertEqual(self.record()["opt_out"], str(off))
        self.uninstall()
        self.assertTrue(off.exists())

    def test_placeholders_stop_before_anything_is_downloaded(self):
        shipped_pkg = re.search(r'^M3_PRO_MESA_PACKAGE="([^"]*)"$', SRC, re.M).group(1)
        shipped_needs = re.search(r'^M3_PRO_MESA_NEEDS="([^"]*)"$', SRC, re.M).group(1)
        bad = [f'M3_PRO_MESA_PACKAGE="mesa-m3-PENDING-aarch64.pkg.tar.zst PENDING-U1-PACKAGE-BUILD"',
               'M3_PRO_MESA_NEEDS="PENDING-U1-INVENTORY"',
               f'M3_PRO_MESA_PACKAGE="{PRO_MESA}"',
               f'M3_PRO_MESA_PACKAGE="mesa-m3-g15g-26.1.4.g15g1-5-aarch64.pkg.tar.zst {"0" * 64}"',
               f'M3_PRO_MESA_PACKAGE="mesa-m3-1-1-aarch64.pkg.tar.zst {"0" * 63}"',
               'M3_PRO_MESA_NEEDS="glibc>=2.43 ;rm"']
        if "PENDING" in shipped_pkg + shipped_needs:
            bad.append(f'M3_PRO_MESA_PACKAGE="{shipped_pkg}"\nM3_PRO_MESA_NEEDS="{shipped_needs}"')
        for env in bad:
            with self.subTest(env=env):
                reset_mac(self, "j516s")
                proc = self.install(env=env, check=False)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("(a packaging mistake). Nothing was installed.", proc.stderr)
                self.assertEqual(self.downloaded(), [])
                self.assertNotIn("pacman -U", self.log())

    def test_a_release_without_the_package_installs_none(self):
        self.mac("j516s")
        self.pro_mesa = ""
        proc = self.install()
        self.assertNotIn("mesa", " ".join(self.downloaded()))
        self.assertEqual(self.mesa_lines(), [])
        self.assertFalse((self.state / "m3-pro-mesa").exists())
        self.assertNotIn("M3 Pro's Mesa", proc.stdout)


class FromEarlierReleasesTest(flow.M3FlowBase):
    """12.0's, 12.1's or 12.2's own script, then this one with no new option: an M3 Pro gets its
    Mesa, and an M1 still gets none."""

    REVS = (("df0c4330", "12.0"), ("1d41e1b7", "12.1"), (REL_12_2, "12.2"))

    def test_an_m3_pro_gets_it_on_a_plain_rerun(self):
        for rev, name in self.REVS:
            with self.subTest(release=name):
                old, _ = old_installer(self, rev, f"install-{name}.sh")
                reset_mac(self, "j516s")
                self.installer = old
                try:
                    self.install()
                finally:
                    self.installer = flow.INSTALLER
                self.assertNotIn(PRO_MESA, self.downloaded())
                self.assertFalse((self.state / "m3-pro-mesa").exists())
                proc = self.install()
                self.assertIn(PRO_MESA, self.downloaded())
                self.assertIn("mesa-m3", (self.fake / "installed").read_text().split())
                rec = dict(l.split("=", 1) for l in (self.state / "m3-pro-mesa").read_text().splitlines())
                self.assertEqual((rec["result"], rec["preexisting"]), ("installed", "none"))
                self.assertIn("take effect at the next login", " ".join(proc.stdout.split()))

    def test_an_m1_still_gets_none(self):
        for rev, name in self.REVS:
            with self.subTest(release=name):
                old, _ = old_installer(self, rev, f"install-{name}.sh")
                reset_mac(self, "j314s")
                self.installer = old
                try:
                    self.install()
                finally:
                    self.installer = flow.INSTALLER
                self.install()
                self.assertNotIn("mesa", self.log())
                self.assertFalse((self.state / "m3-pro-mesa").exists())


class OtherMacsTest(flow.M3FlowBase):
    """Every Mac but the M3 Pro, with this script and with 12.2's: the same commands, in the same
    order, with the same results on disk."""

    BOARDS = [b for b, compat in flow.BOARDS.items() if "apple,t6030" not in compat]

    def setUp(self):
        super().setUp()
        self.old, self.old_version = old_installer(self, REL_12_2, "install-12.2.sh")

    def same(self, run, setup=None, boards=None):
        for board in boards or self.BOARDS:
            with self.subTest(board=board):
                before = run_with(self, self.old, board, run, setup)
                after = run_with(self, flow.INSTALLER, board, run, setup)
                if self.old_version != VERSION:
                    before = {k: (v.replace(self.old_version, VERSION) if k == "log" else v)
                              for k, v in before.items()}
                self.assertEqual(after["codes"], before["codes"])
                self.assertEqual(after["log"].splitlines(), before["log"].splitlines())
                self.assertEqual(sorted(after["tree"]), sorted(before["tree"]))
                for path, data in before["tree"].items():
                    self.assertEqual(after["tree"][path], data, path)
                # Nothing of the M3 Pro's Mesa, by name or on disk.
                self.assertNotIn("state/m3-pro-mesa", after["tree"])
                self.assertNotIn(PRO_MESA, after["log"])
                self.assertNotIn("pacman -Q mesa-m3\n", after["log"] + "\n")
                yield board, after

    def install_and_uninstall(self):
        a = self.run_sh(SUDO_LOG + "M3_TRY=0\ninstall_all", check=False).returncode
        b = self.run_sh(SUDO_LOG + "uninstall_all", check=False).returncode if a == 0 else None
        return (a, b)

    def test_an_install_runs_as_on_12_2(self):
        for board, after in self.same(lambda: self.run_sh(SUDO_LOG + "M3_TRY=0\ninstall_all",
                                                          check=False).returncode):
            # Mesa before the install: none. After it: none, and nothing downloaded or recorded.
            self.assertNotIn("mesa", after["log"])
            self.assertFalse([p for p in after["tree"] if "mesa" in p])
            self.assertFalse([l for l in after["tree"]["fake/installed"].decode().split() if l.startswith("mesa")])

    def test_install_and_uninstall_with_the_owners_mesa(self):
        for board, after in self.same(self.install_and_uninstall, setup=lambda: owner_mesa(self)):
            log = after["log"]
            self.assertNotIn("pacman -Rn --noconfirm mesa-m3", log)
            self.assertFalse([l for l in log.splitlines() if l.startswith(("pacman -U", "pacman -R")) and "mesa" in l])
            installed = after["tree"]["fake/installed"].decode().split()
            self.assertIn("mesa-m3", installed)
            self.assertIn("mesa-m3-g15g", installed)
            self.assertEqual(after["tree"]["opt/mesa-m3/lib/libvulkan_asahi.so"], b"the owner's own build")
            self.assertEqual(after["tree"]["home/.config/chonkstep/m3gpu-session.env"], CHONKSTEP_ENV)

    def test_no_m3_mesa_changes_nothing_elsewhere(self):
        # The option is taken on every Mac (one release, one set of options) and does nothing there.
        def run():
            return self.run_sh(SUDO_LOG + "M3_TRY=0\nM3_PRO_MESA=0\ninstall_all", check=False).returncode
        for board, after in self.same(run, boards=["j314s", "j613", "j516c"]):
            self.assertNotIn("--no-m3-mesa", after["log"])


class ReleaseAssetTest(unittest.TestCase):
    def test_the_package_is_an_asset_of_its_own(self):
        # Never in PACKAGES (what every Mac downloads, and what the lab's common manifest checks).
        block = re.search(r"^PACKAGES=\(\n(.*?)^\)", SRC, re.M | re.S).group(1)
        self.assertNotIn("mesa", block)
        self.assertRegex(SRC, r'(?m)^M3_PRO_MESA_PACKAGE="mesa-m3-')
        self.assertRegex(SRC, r'(?m)^M3_PRO_MESA_PREFIX="/opt/mesa-m3"$')

    def test_nothing_is_written_for_it_outside_the_package(self):
        # The package owns the session integration: this script never writes, links or deletes
        # anything under the prefix, the package's switch-offs or a home directory.
        body = SRC[SRC.index("# ---- the M3 Pro's Mesa"):SRC.index("this_board() {")]
        for bad in (r"\brm\b", r"\bln\b", r"\bmv\b(?! \"\$work)", r"\binstall -", r">\s*\"?\$home",
                    r"\btee\b(?! \"\$STATE/\$M3_PRO_MESA_RECORD_NAME\")"):
            self.assertIsNone(re.search(bad, body), bad)


if __name__ == "__main__":
    unittest.main()
