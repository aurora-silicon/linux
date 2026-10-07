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
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import os
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
# The record: its first line, its keys in order (each once), and the keys that repeat (one line
# per path), which may come after user_setup and opt_out.
SCHEMA = "aurora.m3-pro-mesa-state/1"
RUN_KEYS = ["schema", "run_id", "release", "written_at", "boot_id", "kernel", "board", "installer_sha256",
            "installer_source"]
KEYS = RUN_KEYS + ["package", "version", "file", "sha256", "prefix", "result", "installed_version",
                   "installed_by", "preexisting", "user", "user_setup_source", "user_setup", "opt_out",
                   "opt_out_cmdline", "created_files"]
# An error record: the run's keys, and the ownership history.
ERROR_KEYS = RUN_KEYS + ["package", "result", "record_error", "installed_version", "installed_by", "preexisting"]
REPEATED = {"user_setup_path", "opt_out_path", "record_error_key"}
UUID = r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
RESULTS = ["installed", "current", "newer-kept", "skipped-flag", "skipped-deps", "failed"]
TAG = re.search(r"^TAG=(\S+)$", SRC, re.M).group(1)
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


def same_commands(tc, before, after):
    """The two command logs ran the same commands. The $sudo lines and the others are compared
    each in order, and all lines as a multiset: the two sides of a pipeline such as
    "pacman -Q ... | $sudo tee ..." log in either order."""
    a, b = before.splitlines(), after.splitlines()
    tc.assertEqual(sorted(b), sorted(a))
    tc.assertEqual([l for l in b if l.startswith("sudo ")], [l for l in a if l.startswith("sudo ")])
    tc.assertEqual([l for l in b if not l.startswith("sudo ")], [l for l in a if not l.startswith("sudo ")])


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


def parse_record(tc, data):
    """The record as {key: value} plus {repeated key: [values]}, after checking its form: the
    schema first, key=value lines, every scalar key exactly once and in order, and only the
    declared keys repeated."""
    text = data.decode()
    tc.assertTrue(text.endswith("\n"), text)
    lines = text[:-1].split("\n")
    tc.assertNotIn("\r", text)
    tc.assertEqual(lines[0], f"schema={SCHEMA}")
    scalars, lists, order = {}, {k: [] for k in REPEATED}, []
    for line in lines:
        key, sep, value = line.partition("=")
        tc.assertEqual(sep, "=", line)
        tc.assertRegex(key, r"^[a-z0-9_]+$")
        if key in REPEATED:
            lists[key].append(value)
        else:
            tc.assertNotIn(key, scalars, f"{key} twice")
            scalars[key] = value
            order.append(key)
    tc.assertRegex(scalars["run_id"], f"^{UUID}$")
    if scalars["result"] == "record-error":
        tc.assertEqual(order, ERROR_KEYS)
        tc.assertIn(scalars["record_error"], ("value", "write"))
        tc.assertEqual(bool(lists["record_error_key"]), scalars["record_error"] == "value")
        tc.assertEqual(lists["user_setup_path"] + lists["opt_out_path"], [])
        return scalars, lists
    tc.assertEqual(order, KEYS)
    tc.assertEqual(lists["record_error_key"], [])
    tc.assertIn(scalars["user_setup_source"], ("package-detector", "installer-builtin"))
    if lists["user_setup_path"]:
        tc.assertEqual(scalars["user_setup"], "present")
    if scalars["user_setup_source"] == "installer-builtin" and scalars["user_setup"] != "unknown":
        tc.assertEqual(scalars["user_setup"], "present" if lists["user_setup_path"] else "none")
    tc.assertEqual(scalars["opt_out"],
                   "present" if lists["opt_out_path"] or scalars["opt_out_cmdline"] == "yes" else "none")
    return scalars, lists


def record_warnings(proc):
    """The warnings about the M3 Pro's Mesa record, one string each."""
    blocks = " ".join(proc.stderr.split()).split("warning:")[1:]
    return [b for b in blocks if "m3-pro-mesa" in b or "this run's record" in b]


def summary_line(tc, proc):
    """The summary's record line, as {run_id, result, write}."""
    lines = [l.strip() for l in proc.stdout.splitlines() if l.strip().startswith("m3-pro-mesa record:")]
    tc.assertEqual(len(lines), 1, proc.stdout)
    m = re.fullmatch(rf"m3-pro-mesa record: run_id=({UUID}) result=(\S+) write=(\S+)", lines[0])
    tc.assertTrue(m, lines[0])
    return dict(zip(("run_id", "result", "write"), m.groups()))


class ProMesaTest(flow.M3FlowBase):

    def record(self):
        return self.record_lists()[0]

    def record_lists(self):
        path = self.state / "m3-pro-mesa"
        self.assertEqual(path.stat().st_mode & 0o7777, 0o644)
        return parse_record(self, path.read_bytes())

    def installed(self):
        return (self.fake / "installed").read_text().split()

    def temps(self):
        return sorted(p.name for p in self.state.iterdir() if p.name.startswith(".m3-pro-mesa."))

    def stale(self):
        return sorted(p.name for p in self.state.iterdir() if p.name.startswith("m3-pro-mesa.stale-"))

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
                self.assertEqual(rec["user_setup_source"], "installer-builtin")   # no detector in the fake
                self.assertEqual(rec["user_setup"], "none")
                self.assertEqual(rec["opt_out"], "none")
                self.assertEqual(rec["opt_out_cmdline"], "no")
                self.assertEqual(rec["created_files"], "0")
                self.assertEqual(summary_line(self, proc),
                                 {"run_id": rec["run_id"], "result": "installed", "write": "ok"})
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
        # A home whose path has spaces (and an "=" and a ":"): each path stays whole.
        self.home = home = self.tmp / "home dir/of the=owner: x"
        home.mkdir(parents=True)
        files = {
            ".config/chonkstep/m3gpu-session.env": CHONKSTEP_ENV,
            ".config/environment.d/90-vulkan.conf": b"VK_ICD_FILENAMES=/home/owner/icd.json\n",
            ".config/environment.d/91-ours.conf": b"VK_ICD_FILENAMES=/opt/mesa-m3/share/vulkan/icd.d/x.json\n",
            ".config/hypr/hyprland.conf": b"env = LIBGL_DRIVERS_PATH,/home/owner/dri\n",
            ".drirc": b'<driconf><device><application><option name="dri_driver" value="zink"/>'
                      b'</application></device></driconf>\n',
            ".config/drirc": b"<driconf/>\n",                                       # no driver choice
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
        self.assertIn(f"Left as it is: {self.record()['user']}'s own M3 Mesa setup ({home}/.config/"
                      "environment.d/90-vulkan.conf: sets VK_ICD_FILENAMES;", out)
        self.assertIn(f"{home}/.config/chonkstep/m3gpu-session.env: CHONKSTEP_M3_MESA_PREFIX=/home/owner/src/"
                      "mesa-prefix).", out)
        for rel in (".config/environment.d/90-vulkan.conf", ".config/hypr/hyprland.conf", ".drirc"):
            self.assertIn(f"{home}/{rel}", out)
        self.assertNotIn("91-ours.conf", out)                          # it points at the package's prefix
        self.assertNotIn(f"{home}/.config/drirc", out)
        self.assertIn(f"switched off at login by {home}/.config/mesa-m3/disable", out)
        rec, lists = self.record_lists()
        self.assertEqual(rec["user_setup"], "present")
        self.assertEqual(rec["user_setup_source"], "installer-builtin")
        self.assertEqual(lists["user_setup_path"], [
            f"{home}/.config/environment.d/90-vulkan.conf", f"{home}/.config/hypr/hyprland.conf",
            f"{home}/.drirc", f"{home}/.config/chonkstep/m3gpu-session.env"])
        self.assertEqual(rec["opt_out"], "present")
        self.assertEqual(lists["opt_out_path"], [f"{home}/.config/mesa-m3/disable"])
        self.assertEqual(rec["opt_out_cmdline"], "no")
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
        rec, lists = self.record_lists()
        self.assertEqual((rec["opt_out"], lists["opt_out_path"]), ("present", [str(off)]))
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
               'M3_PRO_MESA_NEEDS="glibc>=2.43 ;rm"',
               'M3_PRO_MESA_DETECTOR="PENDING-U1-DETECTOR"',
               'M3_PRO_MESA_SETUP_LIST="PENDING-U1-SETUP-LIST"',
               'M3_PRO_MESA_DETECTOR="relative/path"']
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


    def test_every_result_writes_one_line_per_key(self):
        # parse_record: the schema first, each scalar key once and in order, only the path keys
        # repeated; for every result, with and without paths to list.
        def setup_paths():
            (self.home / ".drirc").write_text('<option name="dri_driver" value="zink"/>')
            (self.home / ".config/environment.d").mkdir(parents=True, exist_ok=True)
            (self.home / ".config/environment.d/a.conf").write_text("VK_DRIVER_FILES=/x\n")
            (self.tmp / "etc/mesa-m3").mkdir(parents=True, exist_ok=True)
            (self.tmp / "etc/mesa-m3/disable").write_text("")
        runs = {
            "installed": dict(),
            "current": dict(before=lambda: self.owner_has(PRO_MESA_VERSION)),
            "newer-kept": dict(before=lambda: self.owner_has("27.0-1")),
            "skipped-flag": dict(env="M3_PRO_MESA=0"),
            "skipped-deps": dict(extra={"FAKE_GLIBC": "2.0-1"}),
            "failed": dict(extra={"FAKE_FAIL_U_FOR": "mesa-m3-*"}),
        }
        self.assertEqual(sorted(runs), sorted(RESULTS))
        for result, run in runs.items():
            for paths in (False, True):
                with self.subTest(result=result, paths=paths):
                    reset_mac(self, "j516s")
                    if paths:
                        setup_paths()
                    run.get("before", lambda: None)()
                    self.extra_env.update(run.get("extra", {}))
                    try:
                        self.install(env=run.get("env", ""), check=False)
                    finally:
                        for k in run.get("extra", {}):
                            self.extra_env.pop(k)
                    rec, lists = self.record_lists()
                    self.assertEqual(rec["result"], result)
                    self.assertEqual(len(lists["user_setup_path"]), 2 if paths else 0)
                    self.assertEqual(len(lists["opt_out_path"]), 1 if paths else 0)

    def test_the_record_is_bound_to_this_run(self):
        self.mac("j516s")
        before = datetime.now(timezone.utc).replace(microsecond=0)
        self.install()
        rec = self.record()
        self.assertEqual(rec["release"], TAG)
        written = datetime.strptime(rec["written_at"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        self.assertLessEqual(abs((written - before).total_seconds()), 120)
        self.assertEqual(rec["boot_id"], Path("/proc/sys/kernel/random/boot_id").read_text().strip())
        self.assertEqual(rec["kernel"], os.uname().release)
        self.assertEqual(rec["board"], "apple,j516s")
        self.assertEqual(rec["installer_sha256"], hashlib.sha256(flow.INSTALLER.read_bytes()).hexdigest())
        self.assertEqual(rec["installer_source"], "file")
        # A new id for every run, the one the summary line names.
        proc = self.install()
        again = self.record()
        self.assertNotEqual(again["run_id"], rec["run_id"])
        self.assertEqual(summary_line(self, proc)["run_id"], again["run_id"])
        # Through a pipe the script can't be hashed again: no guess.
        self.install(env="SELF_SOURCE=stdin SELF_SHA256=unavailable")
        rec = self.record()
        self.assertEqual((rec["installer_sha256"], rec["installer_source"]), ("unavailable", "stdin"))

    def test_how_the_script_is_read_decides_its_hash(self):
        # The top of the script itself, run as the lab runs it (bash <file>) and as the one-liner
        # does (curl ... | bash): the source-only return is swapped for a print.
        src = SRC.replace('if [[ ${AURORA_SEP_SOURCE_ONLY:-} == 1 ]]; then return 0; fi',
                          'echo "$SELF_SOURCE $SELF_SHA256"; exit 0', 1)
        self.assertNotEqual(src, SRC)
        copy = self.tmp / "dir with space" / "install-aurora-sep.sh"
        copy.parent.mkdir()
        copy.write_text(src)
        sha = hashlib.sha256(src.encode()).hexdigest()
        env = {**os.environ, "AURORA_SEP_SOURCE_ONLY": "", "NO_COLOR": "1"}
        for how, cmd, stdin, want in (
                ("file", ["bash", str(copy)], None, f"file {sha}"),
                ("relative file", ["bash", copy.name], None, f"file {sha}"),
                ("pipe", ["bash"], src, "stdin unavailable"),
                ("pipe with arguments", ["bash", "-s", "--", "--read-only"], src, "stdin unavailable"),
                ("process substitution", ["bash", "-c", f"bash <(cat '{copy}')"], None, "stdin unavailable")):
            with self.subTest(how=how):
                out = subprocess.run(cmd, input=stdin, capture_output=True, text=True, env=env,
                                     cwd=copy.parent).stdout.strip()
                self.assertEqual(out, want)

    def test_the_record_is_written_through_sudo_and_renamed_into_place(self):
        self.mac("j516s")
        self.install(env=SUDO_LOG)
        rec = str(self.state / "m3-pro-mesa")
        tmp_rx = re.escape(str(self.state)) + r"/\.m3-pro-mesa\.[A-Za-z0-9]{6}"
        lines = [l for l in self.log().splitlines() if l.startswith("sudo ") and "m3-pro-mesa" in l]
        self.assertEqual(len(lines), 5, lines)
        self.assertRegex(lines[0], rf"^sudo mktemp {re.escape(str(self.state))}/\.m3-pro-mesa\.XXXXXX$")
        self.assertRegex(lines[1], rf"^sudo tee {tmp_rx}$")
        self.assertRegex(lines[2], rf"^sudo chmod 0644 {tmp_rx}$")
        self.assertRegex(lines[3], rf"^sudo sync {tmp_rx}$")
        self.assertRegex(lines[4], rf"^sudo mv -f {tmp_rx} {re.escape(rec)}$")
        self.assertEqual(lines[1].split()[-1], lines[4].split()[-2])
        log = self.log().splitlines()
        self.assertEqual(log[log.index(lines[4]) + 1], f"sudo sync {self.state}")
        self.assertNotIn("sudo rm", "\n".join(l for l in log if "m3-pro-mesa" in l))
        self.assertEqual([p.name for p in self.state.iterdir() if p.name.startswith(".m3-pro-mesa")], [])

    # Failures, injected with stubs of the commands the writer runs. FIRST fails only the first
    # write of the run (this run's record); ALWAYS fails the error record too.
    FIRST = '[[ ${1:-} == *"/.m3-pro-mesa."* && ! -e $FAKE/failed-once ]] && { touch "$FAKE/failed-once"; %s; }'
    ALWAYS = '[[ ${1:-} == *"/.m3-pro-mesa."* ]] && { %s; }'
    STUBS = {
        "temporary file": ("mktemp", "return 1"),
        "write": ("tee", 'cat >/dev/null; printf partial >"$1"; return 1'),
        "fsync": ("sync", "return 1"),
        "rename": ("mv", "return 1"),
    }

    def stub(self, name, when):
        cmd, body = self.STUBS[name]
        arg = '"${2:-}"' if cmd == "mv" else '"${1:-}"'
        test = (when % body).replace('${1:-}', arg[1:-1])
        (self.fake / "failed-once").unlink(missing_ok=True)
        return f'{cmd}() {{ {test}; command {cmd} "$@"; }}'

    def test_a_failure_before_the_rename_leaves_an_error_record(self):
        # This run's record fails before its rename, the error record works: the earlier record
        # is gone, an error record (record_error=write) is current, and the exit status is 4.
        for name in self.STUBS:
            with self.subTest(fails=name):
                reset_mac(self, "j516s")
                self.install()
                proc = self.install(env=self.stub(name, self.FIRST) + "\nM3_PRO_MESA=0", check=False)
                self.assertEqual(proc.returncode, 4, proc.stderr)
                rec, lists = self.record_lists()
                self.assertEqual((rec["result"], rec["record_error"]), ("record-error", "write"))
                self.assertEqual(lists["record_error_key"], [])
                self.assertEqual((rec["installed_by"], rec["preexisting"]), ("installer", "none"))
                err = " ".join(proc.stderr.split())
                self.assertEqual(len(record_warnings(proc)), 1, proc.stderr)
                self.assertEqual(err.count("did not write this run's record"), 1, proc.stderr)
                self.assertIn("An error record (result=record-error) replaces", err)
                self.assertEqual(summary_line(self, proc),
                                 {"run_id": rec["run_id"], "result": "record-error", "write": "error-record"})
                self.assertIn("The exit status is 4.", proc.stdout)
                self.assertEqual(self.temps(), [])
                self.assertEqual(self.stale(), [])

    def test_when_no_record_can_be_written_the_previous_one_is_moved_aside(self):
        for name in self.STUBS:
            with self.subTest(fails=name):
                reset_mac(self, "j516s")
                self.install()
                before = (self.state / "m3-pro-mesa").read_bytes()
                written = re.search(rb"^written_at=(\S+)$", before, re.M).group(1).decode()
                proc = self.install(env=self.stub(name, self.ALWAYS) + "\nM3_PRO_MESA=0", check=False)
                self.assertEqual(proc.returncode, 4, proc.stderr)
                self.assertFalse((self.state / "m3-pro-mesa").exists())
                self.assertEqual(self.stale(), [f"m3-pro-mesa.stale-{written}"])
                self.assertEqual((self.state / f"m3-pro-mesa.stale-{written}").read_bytes(), before)
                err = " ".join(proc.stderr.split())
                self.assertEqual(len(record_warnings(proc)), 1, proc.stderr)
                # Both failures, in the one warning.
                self.assertIn("this run's record: ", err)
                self.assertIn("; the error record: ", err)
                self.assertTrue(err.rstrip().endswith(f"The previous record is now {self.state}/m3-pro-mesa.stale-{written}, "
                                                      "so no record is current."), err)
                self.assertEqual(summary_line(self, proc)["write"], "none")
                self.assertEqual(summary_line(self, proc)["result"], "none")
                self.assertEqual(self.temps(), [])
        # With no earlier record: nothing to move, still a warning and 4.
        reset_mac(self, "j516s")
        proc = self.install(env=self.stub("rename", self.ALWAYS), check=False)
        self.assertEqual(proc.returncode, 4)
        self.assertIn("There is no record.", " ".join(proc.stderr.split()))
        self.assertEqual(os.listdir(self.state).count("m3-pro-mesa"), 0)

    def test_a_stale_record_is_never_overwritten(self):
        self.mac("j516s")
        self.install()
        before = (self.state / "m3-pro-mesa").read_bytes()
        written = re.search(rb"^written_at=(\S+)$", before, re.M).group(1).decode()
        taken = {f"m3-pro-mesa.stale-{written}": b"an earlier stale record\n",
                 f"m3-pro-mesa.stale-{written}.1": b"another\n"}
        for name, data in taken.items():
            (self.state / name).write_bytes(data)
        proc = self.install(env=self.stub("rename", self.ALWAYS) + "\nM3_PRO_MESA=0", check=False)
        self.assertEqual(proc.returncode, 4)
        for name, data in taken.items():
            self.assertEqual((self.state / name).read_bytes(), data)
        self.assertEqual((self.state / f"m3-pro-mesa.stale-{written}.2").read_bytes(), before)
        self.assertFalse((self.state / "m3-pro-mesa").exists())
        # The fsync after the move fails: still said.
        self.install()
        stub = self.stub("rename", self.ALWAYS) + '\nsync() { [[ ${1:-} == "$STATE" ]] && return 1; command sync "$@"; }'
        proc = self.install(env=stub + "\nM3_PRO_MESA=0", check=False)
        self.assertEqual(proc.returncode, 4)
        self.assertIn(f"But the fsync of {self.state} failed after the move.", " ".join(proc.stderr.split()))
        # And when the move itself fails: the earlier record stays, named as an earlier run's.
        self.install()
        before = (self.state / "m3-pro-mesa").read_bytes()
        stub = self.stub("rename", self.ALWAYS) + '\nmv() { [[ ${1:-} == -n ]] && return 1; ' \
            '[[ ${2:-} == *"/.m3-pro-mesa."* ]] && return 1; command mv "$@"; }'
        proc = self.install(env=stub + "\nM3_PRO_MESA=0", check=False)
        self.assertEqual(proc.returncode, 4)
        self.assertEqual((self.state / "m3-pro-mesa").read_bytes(), before)
        err = " ".join(proc.stderr.split())
        self.assertIn("could not move the previous record aside", err)
        self.assertIn("is an earlier run's record, not this run's", err)
        self.assertEqual(summary_line(self, proc)["write"], "stale")

    def test_a_failed_directory_fsync_after_the_rename_is_reported(self):
        # The new record is in place, but its durability is not confirmed: said, and exit 4.
        self.mac("j516s")
        self.install()
        dirsync = 'sync() { [[ ${1:-} == "$STATE" ]] && return 1; command sync "$@"; }'
        proc = self.install(env=dirsync + "\nM3_PRO_MESA=0", check=False)
        self.assertEqual(proc.returncode, 4, proc.stderr)
        rec = self.record()
        self.assertEqual(rec["result"], "skipped-flag")                 # this run's record, published
        err = " ".join(proc.stderr.split())
        self.assertIn(f"wrote {self.state}/m3-pro-mesa, but its durability is not confirmed: fsync of {self.state} "
                      "failed after the rename", err)
        self.assertEqual(summary_line(self, proc),
                         {"run_id": rec["run_id"], "result": "skipped-flag", "write": "unsynced"})
        self.assertIn("was written, but its durability is not confirmed", " ".join(proc.stdout.split()))
        # Distinct from a failure before the rename (test above): there no new record exists.

    def test_an_interrupted_write_keeps_the_previous_record(self):
        self.mac("j516s")
        self.install()
        before = (self.state / "m3-pro-mesa").read_bytes()
        # Killed after the new record is written to its temporary file, before the rename.
        self.extra_env["TMPDIR"] = str(self.tmp)
        proc = self.install(env='sync() { [[ ${1:-} == *"/.m3-pro-mesa."* ]] && kill -9 $$; command sync "$@"; }\n'
                                'M3_PRO_MESA=0', check=False)
        self.assertEqual(proc.returncode, -9)
        self.assertEqual((self.state / "m3-pro-mesa").read_bytes(), before)
        left = self.temps()
        self.assertEqual(len(left), 1)
        leftover = (self.state / left[0]).read_bytes()
        self.assertIn(b"result=skipped-flag", leftover)
        # The next run writes its own record and leaves the leftover alone, in one line.
        proc = self.install()
        self.assertEqual(self.temps(), left)
        self.assertEqual((self.state / left[0]).read_bytes(), leftover)
        self.assertIn("Left alone: 1 temporary record file(s) of another or an interrupted run", proc.stdout)
        self.assertEqual(self.record()["result"], "current")

    def test_another_runs_temporary_file_is_left_alone(self):
        self.mac("j516s")
        other = self.state / ".m3-pro-mesa.AbC123"
        other.write_bytes(b"schema=aurora.m3-pro-mesa-state/1\nanother writer, still writing\n")
        proc = self.install()
        self.assertEqual(other.read_bytes(), b"schema=aurora.m3-pro-mesa-state/1\nanother writer, still writing\n")
        self.assertEqual(self.temps(), [".m3-pro-mesa.AbC123"])
        self.assertEqual(self.record()["result"], "installed")
        self.assertEqual(proc.stdout.count("Left alone: 1 temporary record file(s)"), 1)
        # Also when this run's own write fails: only its own temporary file goes.
        proc = self.install(env=self.stub("rename", self.FIRST), check=False)
        self.assertEqual(self.temps(), [".m3-pro-mesa.AbC123"])

    def test_a_newline_or_carriage_return_in_any_value_gives_an_error_record(self):
        self.mac("j516s")
        self.install()
        bad_file = self.home / ".config/environment.d/a\nuser_setup=none.conf"
        bad_file.parent.mkdir(parents=True)
        bad_file.write_text("VK_ICD_FILENAMES=/x\n")
        for case, extra, key, quoted in (
                ("a path with a newline", {}, "user_setup_path", "user_setup_path=$'"),
                ("a user name with a carriage return", {"SUDO_USER": "own\rer"}, "user", "user=$'own\\rer'")):
            with self.subTest(case=case):
                if case.startswith("a user"):
                    bad_file.unlink()
                self.extra_env.update(extra)
                proc = self.install(env="M3_PRO_MESA=0", check=False)
                for k in extra:
                    self.extra_env.pop(k)
                self.assertEqual(proc.returncode, 4)
                data = (self.state / "m3-pro-mesa").read_bytes()
                rec, lists = parse_record(self, data)
                self.assertEqual((rec["result"], rec["record_error"]), ("record-error", "value"))
                self.assertEqual(lists["record_error_key"], [key])
                # Never the refused value.
                self.assertNotIn(b"user_setup=none.conf", data)
                self.assertNotIn(b"own", data)
                err = " ".join(proc.stderr.split())
                self.assertEqual(len(record_warnings(proc)), 1, proc.stderr)
                self.assertIn("a value holds a newline or a carriage return", err)
                self.assertIn(quoted, err)
        # Refused, and the error record fails too: moved aside.
        self.extra_env["SUDO_USER"] = "own\rer"
        proc = self.install(env=self.stub("rename", self.ALWAYS) + "\nM3_PRO_MESA=0", check=False)
        self.extra_env.pop("SUDO_USER")
        self.assertEqual(proc.returncode, 4)
        self.assertFalse((self.state / "m3-pro-mesa").exists())
        self.assertEqual(len(self.stale()), 1)

    def test_after_an_error_record(self):
        # --uninstall keeps mesa-m3 and says why; the next install keeps the ownership history.
        self.mac("j516s")
        self.install()
        self.extra_env["SUDO_USER"] = "own\rer"
        self.install(env="M3_PRO_MESA=0", check=False)
        self.extra_env.pop("SUDO_USER")
        self.assertEqual(self.record()["result"], "record-error")
        since = len(self.log())
        proc = self.uninstall()
        self.assertEqual(self.mesa_lines(since), [])
        self.assertIn("the last install could not write its record (result=record-error)", " ".join(proc.stdout.split()))
        self.assertIn("mesa-m3", self.installed())
        # Again, with an error record in place: the next good run is this script's again.
        reset_mac(self, "j516s")
        self.install()
        self.extra_env["SUDO_USER"] = "own\rer"
        self.install(env="M3_PRO_MESA=0", check=False)
        self.extra_env.pop("SUDO_USER")
        proc = self.install()
        rec = self.record()
        self.assertEqual((rec["result"], rec["installed_by"], rec["preexisting"]), ("current", "installer", "none"))
        self.assertEqual(proc.returncode, 0)

    def test_a_record_of_another_schema_is_not_read(self):
        # Not this schema (an earlier draft, or anything else): never read, never trusted for
        # removal; the next install writes a new one.
        self.mac("j516s")
        self.install()
        rec = self.state / "m3-pro-mesa"
        body = rec.read_text().split("\n", 1)[1]
        rec.write_text(body)            # no schema line
        since = len(self.log())
        proc = self.uninstall()
        self.assertEqual(self.mesa_lines(since), [])
        self.assertIn("is not a record this script reads", " ".join(proc.stderr.split()))
        self.assertIn("mesa-m3", self.installed())
        reset_mac(self, "j516s")
        self.owner_has(PRO_MESA_VERSION)
        rec.write_text("schema=aurora.m3-pro-mesa-state/2\ninstalled_by=installer\npreexisting=none\n"
                       f"installed_version={PRO_MESA_VERSION}\n")
        self.install()
        new = self.record()
        self.assertEqual((new["installed_by"], new["preexisting"]), ("owner", PRO_MESA_VERSION))

    def test_a_version_changed_outside_this_script_is_the_owners(self):
        self.mac("j516s")
        self.install()
        self.owner_has("26.1.4.mine-1")     # replaced outside this script: no longer this script's
        self.install(env="M3_PRO_MESA=0")
        self.assertEqual(self.record()["installed_by"], "owner")
        since = len(self.log())
        self.uninstall()
        self.assertEqual(self.mesa_lines(since), [])


    def detector(self, body):
        path = self.tmp / "opt/mesa-m3/libexec/mesa-m3-user-setup"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/bash\n" + 'echo "$*" >"$FAKE/detector-args"\n' + body)
        path.chmod(0o755)

    # mesa-m3-user-setup's answer (U1's report, section 4.5): key=value lines; exit 0 none, 1 present.
    PRESENT = ("echo schema=aurora.mesa-m3-user-setup/1; echo user_setup=present\n"
               "echo 'user_setup_path=/a path/from the detector'\n"
               "echo 'user_setup_path=/b'\n"
               "echo 'user_setup_detail=/a path/from the detector sets GALLIUM_DRIVER'\n"
               "echo 'user_setup_detail=/b: chonkstep'\\''s M3 Mesa prefix is /x'\nexit 1\n")
    NONE = "echo schema=aurora.mesa-m3-user-setup/1; echo user_setup=none; exit 0\n"

    def test_the_user_setup_comes_from_the_packages_detector_once_installed(self):
        self.mac("j516s")
        self.detector(self.PRESENT)
        (self.home / ".drirc").write_text('<option name="dri_driver" value="zink"/>')   # the built-in check's
        proc = self.install()
        rec, lists = self.record_lists()
        self.assertEqual((rec["user_setup_source"], rec["user_setup"]), ("package-detector", "present"))
        self.assertEqual(lists["user_setup_path"], ["/a path/from the detector", "/b"])
        self.assertEqual((self.fake / "detector-args").read_text().strip(), f"--home {self.home}")
        out = " ".join(proc.stdout.split())
        self.assertIn("/a path/from the detector: sets GALLIUM_DRIVER; /b: chonkstep's M3 Mesa prefix is /x", out)
        self.detector(self.NONE)
        self.install()
        rec, lists = self.record_lists()
        self.assertEqual((rec["user_setup_source"], rec["user_setup"], lists["user_setup_path"]),
                         ("package-detector", "none", []))

    def test_the_built_in_check_without_mesa_m3_or_its_detector(self):
        # Not installed (--no-m3-mesa), or installed without a detector, or the detector fails.
        self.mac("j516s")
        (self.home / ".drirc").write_text('<option name="dri_driver" value="zink"/>')
        self.detector(self.NONE)
        self.install(env="M3_PRO_MESA=0")
        self.assertFalse((self.fake / "detector-args").exists())
        rec, lists = self.record_lists()
        self.assertEqual((rec["user_setup_source"], lists["user_setup_path"]),
                         ("installer-builtin", [f"{self.home}/.drirc"]))
        self.detector("exit 2\n")
        proc = self.install()
        rec, lists = self.record_lists()
        self.assertEqual((rec["user_setup_source"], lists["user_setup_path"]),
                         ("installer-builtin", [f"{self.home}/.drirc"]))
        self.assertIn("mesa-m3-user-setup failed (exit 2)", " ".join(proc.stderr.split()))
        for body in ("echo schema=other/1; exit 0\n",                       # another form
                     "echo schema=aurora.mesa-m3-user-setup/1; echo user_setup=present; exit 0\n",  # inconsistent
                     self.NONE.replace("exit 0", "exit 1")):
            with self.subTest(detector=body):
                self.detector(body)
                self.install()
                self.assertEqual(self.record()["user_setup_source"], "installer-builtin")
        # And when the built-in check fails too: unknown, never "none".
        proc = self.install(env="m3_pro_mesa_builtin_setup() { return 5; }")
        rec, lists = self.record_lists()
        self.assertEqual((rec["user_setup"], lists["user_setup_path"]), ("unknown", []))
        self.assertIn("own check of", " ".join(proc.stderr.split()))

    def builtin(self, home=None):
        proc = self.run_sh(f"m3_pro_mesa_builtin_setup '{home or self.home}'")
        out = proc.stdout.split("\0")
        self.assertEqual(out[-1], "")
        return list(zip(out[:-1:2], out[1:-1:2]))

    def test_the_built_in_check_follows_the_list(self):
        rules = re.search(r'^M3_PRO_MESA_SETUP_RULES="([^"]*)"$', SRC, re.M).group(1).splitlines()
        variables = [r.split()[1] for r in rules if r.startswith("variable ")]
        self.assertEqual(len(variables), 13)
        home, cfg, root = self.home, self.home / ".config", self.tmp / "sysroot"
        envd = cfg / "environment.d"
        envd.mkdir(parents=True)
        # Each variable, set to something else: a finding. Pointing into the prefix, in a comment,
        # or a longer name: none.
        for v in variables:
            (envd / f"{v}.conf").write_text(f"{v}=/somewhere/else\n")
            (envd / f"ours-{v}.conf").write_text(f"{v}=/opt/mesa-m3/lib/x\n")
            (envd / f"ours2-{v}.conf").write_text(f'export {v}="/opt/mesa-m3"\n')
            (envd / f"comment-{v}.conf").write_text(f"# {v}=/somewhere/else\n-- {v}=x\n")
            (envd / f"longer-{v}.conf").write_text(f"MY_{v}_X=/somewhere/else\n")
        found = dict(self.builtin())
        self.assertEqual(found, {f"{envd}/{v}.conf": f"sets {v}" for v in variables})
        for f in envd.iterdir():
            f.unlink()
        # LD_LIBRARY_PATH: a directory with Mesa libraries other than PREFIX/lib.
        mesa = home / "mesa/lib"
        (mesa / "dri").mkdir(parents=True)
        (mesa / "dri/zink_dri.so").write_text("")
        (home / "empty/lib").mkdir(parents=True)
        (root / "opt/mesa-m3/lib").mkdir(parents=True)
        (root / "opt/mesa-m3/lib/libgallium-26.so").write_text("")
        (root / "usr/local/mesa").mkdir(parents=True)
        (root / "usr/local/mesa/libEGL_mesa.so.0").write_text("")
        (envd / "a.conf").write_text("LD_LIBRARY_PATH=$HOME/mesa/lib:/opt/mesa-m3/lib\n")
        (envd / "b.conf").write_text("LD_LIBRARY_PATH=${HOME}/empty/lib:/opt/mesa-m3/lib\n")
        (envd / "c.conf").write_text('LD_LIBRARY_PATH="/usr/local/mesa"\n')
        (cfg / "hypr").mkdir()
        (cfg / "hypr/envs.lua").write_text('-- LD_LIBRARY_PATH=~/mesa/lib\nhl.env("LD_LIBRARY_PATH", "~/empty/lib")\n')
        found = self.builtin()
        self.assertEqual(found, [(f"{envd}/a.conf", f"puts another Mesa in LD_LIBRARY_PATH ({mesa})"),
                                 (f"{envd}/c.conf", "puts another Mesa in LD_LIBRARY_PATH (/usr/local/mesa)")])
        for f in list(envd.iterdir()) + [cfg / "hypr/envs.lua"]:
            f.unlink()
        # Every envfile of the list, the user's and the system's (under the root).
        files = [cfg / "uwsm/env", cfg / "uwsm/env-hyprland", cfg / "uwsm/env.d/x", cfg / "uwsm/env-hyprland.d/y",
                 cfg / "uwsm/default",
                 cfg / "hypr/a.conf", cfg / "hypr/b.lua", root / "etc/xdg/uwsm/env",
                 root / "etc/xdg/uwsm/env-x", root / "etc/xdg/uwsm/env.d/y", root / "etc/environment"]
        for f in files:
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("export GALLIUM_DRIVER=zink\n")
        self.assertEqual([p for p, _ in self.builtin()], [str(f) for f in files])
        for f in files:
            f.unlink()
        # drirc: only with a dri_driver option.
        for f in (home / ".drirc", cfg / "drirc", root / "etc/drirc"):
            f.write_text('<option name="dri_driver" value="zink"/>')
        (cfg / "drirc").write_text("<driconf/>")
        self.assertEqual(self.builtin(), [(str(home / ".drirc"), "chooses a driver (dri_driver)"),
                                          (str(root / "etc/drirc"), "chooses a driver (dri_driver)")])
        for f in (home / ".drirc", cfg / "drirc", root / "etc/drirc"):
            f.unlink()
        # chonkstep: another prefix (quoted, commented, CRLF); the package's; none set.
        chonk = cfg / "chonkstep/m3gpu-session.env"
        chonk.parent.mkdir(parents=True)
        for text, want in (('export CHONKSTEP_M3_MESA_PREFIX="/home/x/p" # mine\r\n', "CHONKSTEP_M3_MESA_PREFIX=/home/x/p"),
                           ("CHONKSTEP_M3_MESA_PREFIX=/home/x/q\nCHONKSTEP_M3_MESA_PREFIX=/home/x/r\n",
                            "CHONKSTEP_M3_MESA_PREFIX=/home/x/r"),
                           ("CHONKSTEP_M3_MESA_PREFIX=/opt/mesa-m3\n", None),
                           ("CHONKSTEP_M3_MESA_PREFIX='/opt/mesa-m3/'\n", None),
                           ("CHONKSTEP_M3_CLIENTS=gpu\n", "sets no CHONKSTEP_M3_MESA_PREFIX: chonkstep's M3 launcher "
                                                         "uses its own default prefix")):
            with self.subTest(chonkstep=text):
                chonk.write_text(text)
                self.assertEqual(self.builtin(), [(str(chonk), want)] if want else [])
        # Nothing for a home with none of it, or no home.
        chonk.unlink()
        self.assertEqual(self.builtin(), [])
        self.assertEqual(self.builtin(self.tmp / "no such home"), [])


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
                same_commands(self, before["log"], after["log"])
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
        # anything under the prefix, the package's switch-offs or a home directory. Its only
        # writes are the set-aside in its work directory and the record's publication in $STATE.
        body = SRC[SRC.index("# ---- the M3 Pro's Mesa"):SRC.index("this_board() {")]
        code = [l.strip() for l in body.splitlines() if not l.strip().startswith("#")]
        allowed = {
            'if ! tmp=$($sudo mktemp "$STATE/.$M3_PRO_MESA_RECORD_NAME.XXXXXX"); then',
            'if ! printf \'%s\\n\' "$@" | $sudo tee "$tmp" >/dev/null; then',
            'elif ! $sudo chmod 0644 "$tmp"; then',
            'elif ! $sudo mv -f "$tmp" "$rec"; then',
            '$sudo rm -f "$tmp" || M3_PRO_MESA_WRITE_ERR+="; $tmp could not be removed"',
            '$sudo mv -n "$rec" "$name" || true',
            'mkdir -p "$work/m3-pro"',
            'mv "$work/$file" "$work/m3-pro/"',
        }
        # Commands in command position (line start, after a pipe, a list operator, if, then or $( ),
        # and redirections into a variable's path.
        verb = (r"(^|&&|\|\||\||;|\bthen\b|\b(el)?if !?|\$\()\s*(\$sudo\s+)?"
                r"(rm|ln|mv|tee|mktemp|chmod|chown|mkdir|cp|install|touch|truncate|dd|sed -i)\b")
        writes = [l for l in code if re.search(verb, l) or re.search(r"[^2]>\s*\"?\$(?!\(|\{?work)", l)]
        self.assertEqual(sorted(set(writes) - allowed), [])
        self.assertEqual(sorted(allowed - set(writes)), [])


if __name__ == "__main__":
    unittest.main()
