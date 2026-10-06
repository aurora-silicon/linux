"""Release guards and upgrades from the previous release.

ReleaseGuardTest fails while any package checksum is still a placeholder:
a release is cut only when it passes. RealM1n1PackageTest checks the
m1n1 package this release names, by its file name always, and by its
content when the file is at hand (AURORA_M1N1_PKG, or the release staging
directory). ReleaseUrlTest and StagedCopyTest cover the staging or mirror
override (AURORA_RELEASE_URL, AURORA_RELEASES_API): the default is the
release's own tag, a staged copy installs through the same download and
checksum loop, and the commands an install prints name the public release.
UpgradeTest runs 11.38's own script on the fake Mac of
test_m3_flow, then this one, as an owner updating would.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest

import test_m3_flow as flow

SRC = flow.SRC
VERSION = flow.VERSION
M1N1_PACKAGE = re.search(r'^M1N1_PACKAGE="(\S+) (\S+)"$', SRC, re.M)
M1N1_BIN_SHA = re.search(r"^M1N1_BIN_SHA=(\S+)$", SRC, re.M).group(1)
# Every m1n1 a release put on a Mac before this one.
EARLIER_M1N1 = ["1.6.1.aurora3-1", "1.6.1.aurora7-1", "1.6.1.aurora8.4-1", "1.6.1.aurora8.5-2"]


def package_entries():
    """Every "file sha256" this script downloads."""
    block = re.search(r"^PACKAGES=\(\n(.*?)^\)", SRC, re.M | re.S).group(1)
    entries = re.findall(r'^\s*"(\S+) (\S+)"$', block, re.M)
    entries.append((M1N1_PACKAGE.group(1), M1N1_PACKAGE.group(2)))
    return entries


def staged_m1n1():
    name = M1N1_PACKAGE.group(1)
    candidates = [os.environ.get("AURORA_M1N1_PKG", "")]
    stage = Path.home() / "source/aurora-recipes" / f"stage-{VERSION.split('-')[-1]}"
    candidates.append(str(stage / name))
    for c in candidates:
        if c and Path(c).is_file() and Path(c).name == name:
            return Path(c)
    return None


class ReleaseGuardTest(unittest.TestCase):
    def test_no_placeholder_checksums(self):
        # PENDING-* stands in for a lab build's sha256 until it exists. The
        # script refuses such a download, so nothing installs; this test is
        # what stops the release from being cut with one.
        pending = [f"{f} {sha}" for f, sha in package_entries() if not re.fullmatch(r"[0-9a-f]{64}", sha)]
        if not re.fullmatch(r"[0-9a-f]{64}", M1N1_BIN_SHA):
            pending.append(f"M1N1_BIN_SHA={M1N1_BIN_SHA}")
        self.assertEqual(pending, [], "placeholders left in install-aurora-sep.sh")

    def test_packages_follow_version(self):
        names = [f for f, _ in package_entries()]
        self.assertIn(f"linux-aurora-$VERSION-aarch64.pkg.tar.zst", names)
        self.assertIn(f"linux-aurora-headers-$VERSION-aarch64.pkg.tar.zst", names)
        self.assertEqual(len([n for n in names if n.startswith("m1n1-")]), 1, names)


class RealM1n1PackageTest(unittest.TestCase):
    def test_name_and_version(self):
        name = M1N1_PACKAGE.group(1)
        m = re.fullmatch(r"m1n1-aurora-(1\.6\.1\.aurora[0-9.]+)-(\d+)-aarch64\.pkg\.tar\.zst", name)
        self.assertTrue(m, name)
        if not shutil.which("vercmp"):
            self.skipTest("vercmp (pacman) is needed to order the versions")
        new = f"{m.group(1)}-{m.group(2)}"
        for old in EARLIER_M1N1:
            with self.subTest(old=old):
                out = subprocess.run(["vercmp", new, old], capture_output=True, text=True).stdout.strip()
                self.assertEqual(out, "1", f"{new} must sort above {old}")

    def test_the_package_itself(self):
        path = staged_m1n1()
        if path is None:
            self.skipTest(f"{M1N1_PACKAGE.group(1)} is not at hand (set AURORA_M1N1_PKG)")
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), M1N1_PACKAGE.group(2))
        with tarfile.open(path) as t:
            info = t.extractfile(".PKGINFO").read().decode()
            m1n1 = t.extractfile("usr/lib/asahi-boot/m1n1.bin").read()
        version = re.search(r"^pkgver = (\S+)$", info, re.M).group(1)
        self.assertIn(f"-{version}-aarch64", M1N1_PACKAGE.group(1))
        self.assertIn("conflict = m1n1", info)
        self.assertRegex(info, r"provides = m1n1=1\.6\.1")
        # The bytes this release names, and the version they report.
        self.assertEqual(hashlib.sha256(m1n1).hexdigest(), M1N1_BIN_SHA)
        tag = "v1.6.1-omarchy." + version.split("-")[0][len("1.6.1."):]
        self.assertIn(tag.encode() + b"\0", m1n1)
        # Every switch an M3 Pro or Air arms, as whole strings.
        strings = set(m1n1.split(b"\0"))
        for name in flow.SWITCH_NAMES:
            self.assertIn(name.encode(), strings, name)
        # The file part stays clear of the M3 display log buffer.
        self.assertFalse(any(m1n1[0x120000:0x180000]))


TAG = re.search(r"^TAG=(\S+)$", SRC, re.M).group(1)
DEFAULT_RELEASE_URL = f"https://github.com/iconidentify/aurora-linux/releases/download/{TAG}"
DEFAULT_RELEASES_API = "https://api.github.com/repos/iconidentify/aurora-linux/releases"
OVERRIDES = ("AURORA_RELEASE_URL", "AURORA_RELEASES_API")


def sourced(body, **env_extra):
    """Runs body after sourcing the script, with only the overrides given."""
    env = {k: v for k, v in os.environ.items() if k not in OVERRIDES}
    env.update(env_extra)
    script = f"set -euo pipefail\nAURORA_SEP_SOURCE_ONLY=1 source '{flow.INSTALLER}'\n{body}"
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env)


class ReleaseUrlTest(unittest.TestCase):
    """AURORA_RELEASE_URL and AURORA_RELEASES_API, the staging or mirror override."""

    def urls(self, **env):
        proc = sourced('printf "%s\\n" "$RELEASE_URL" "$RELEASES_API"', **env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout.splitlines()

    def test_default(self):
        self.assertEqual(self.urls(), [DEFAULT_RELEASE_URL, DEFAULT_RELEASES_API])

    def test_empty_is_the_default(self):
        self.assertEqual(self.urls(AURORA_RELEASE_URL="", AURORA_RELEASES_API=""),
                         [DEFAULT_RELEASE_URL, DEFAULT_RELEASES_API])

    def test_override(self):
        self.assertEqual(self.urls(AURORA_RELEASE_URL="file:///srv/stage", AURORA_RELEASES_API="http://127.0.0.1:8000/api"),
                         ["file:///srv/stage", "http://127.0.0.1:8000/api"])

    def test_printed_commands_name_the_public_release(self):
        # The commands an install prints stay on the public tag under an override.
        proc = sourced('printf "%s\\n" "$PUBLIC_RELEASE_URL"',
                       AURORA_RELEASE_URL="file:///srv/stage", AURORA_RELEASES_API="http://127.0.0.1:8000/api")
        self.assertEqual(proc.stdout.splitlines(), [DEFAULT_RELEASE_URL], proc.stderr)
        self.assertNotRegex(SRC, r"echo .*\$RELEASE_URL")

    def test_releases_api_override(self):
        if not shutil.which("curl"):
            self.skipTest("curl is needed")
        with tempfile.TemporaryDirectory() as d:
            latest = Path(d) / "latest"
            api = Path(d).as_uri()
            latest.write_text('{"tag_name": "%s"}' % TAG)
            proc = sourced("newer_release", AURORA_RELEASES_API=api)
            self.assertEqual((proc.returncode, proc.stdout), (0, ""), proc.stderr)
            latest.write_text('{"tag_name": "sep-7.1.12.aurora2-99.0"}')
            proc = sourced("newer_release", AURORA_RELEASES_API=api)
            self.assertEqual(proc.stdout.strip(), "sep-7.1.12.aurora2-99.0", proc.stderr)


class StagedCopyTest(flow.M3FlowBase):
    """install_all with the real curl against a staged copy (file://)."""

    def setUp(self):
        super().setUp()
        if not shutil.which("curl"):
            self.skipTest("curl is needed")
        # The real curl, not the harness's stub.
        (self.tmp / "bin/curl").unlink()
        self.stage = self.tmp / "pkgs"
        self.extra_env["AURORA_RELEASE_URL"] = self.stage.as_uri()

    def test_install_from_a_staged_copy(self):
        self.mac("j293")
        proc = self.install()
        # Every command it prints names the public release, not the stage.
        printed = proc.stdout + proc.stderr
        script = f"curl -fsSL {DEFAULT_RELEASE_URL}/install-aurora-sep.sh | bash -s -- "
        self.assertIn(script + "--agent-prompt", printed)
        self.assertIn("To undo everything:    " + script + "--uninstall", printed)
        commands = [l for l in printed.splitlines() if "curl " in l]
        self.assertTrue(commands)
        for line in commands:
            self.assertIn("https://github.com/iconidentify/aurora-linux/releases/", line)
            self.assertNotIn(self.stage.as_uri(), line)
        self.assertTrue(self.boot.read_bytes().startswith(b"M1N1:" + flow.M1N1_BASE.encode() + b"\n"))
        self.assertIn("linux-aurora", (self.fake / "installed").read_text().split())

    def test_a_staged_copy_is_still_checked(self):
        self.mac("j293")
        with open(self.stage / "libfprint-1.94.100-1.1-aarch64.pkg.tar.zst", "ab") as f:
            f.write(b"x")
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("does not match its published checksum", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), b"M1N1:original\n")
        self.assertNotIn("pacman -U", self.log())

    def test_a_file_missing_from_the_staged_copy(self):
        self.mac("j293")
        (self.stage / "aurora-touchid-20261003-1-any.pkg.tar.zst").unlink()
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("could not download aurora-touchid-20261003-1-any.pkg.tar.zst", proc.stderr)
        self.assertNotIn("pacman -U", self.log())


class UpgradeTest(flow.M3FlowBase):
    """11.38's own script, then this one, on one fake Mac."""

    OLD_REV = "d14f756c"

    def setUp(self):
        super().setUp()
        old = subprocess.run(["git", "show", f"{self.OLD_REV}:tools/aurora-sep/install-aurora-sep.sh"],
                             cwd=flow.INSTALLER.parent, capture_output=True)
        if old.returncode:
            self.skipTest(f"git can't show 11.38's script ({self.OLD_REV})")
        self.old = self.tmp / "install-11.38.sh"
        self.old.write_bytes(old.stdout)
        self.old_version = re.search(rb"^VERSION=(\S+)$", old.stdout, re.M).group(1).decode()
        # Stand-ins for 11.38's m1n1 packages: aurora3 for M1/M2, aurora7 for M3.
        self.aurora3 = "m1n1-aurora-1.6.1.aurora3-1-aarch64.pkg.tar.zst"
        self.aurora7 = "m1n1-aurora-1.6.1.aurora7-1-aarch64.pkg.tar.zst"
        self.fixture(self.aurora3, None, [])
        self.fixture(self.aurora7, None, ["asahi,t6030-gpu", "asahi,t6030-dcp", "asahi,t6030-dcpext"])

    def install_1138(self, try_=0):
        self.installer = self.old
        try:
            return self.run_sh(f'PACKAGES+=("{self.aurora3} {self.shas[self.aurora3]}")\n'
                               f'M3_M1N1_PACKAGE="{self.aurora7} {self.shas[self.aurora7]}"\n'
                               f"M3_TRY={try_}\ninstall_all")
        finally:
            self.installer = flow.INSTALLER

    def kept(self, version):
        return self.boot.parent / f"boot.bin.before-{version}"

    def test_j516s_from_11_38(self):
        self.mac("j516s")
        self.install_1138()
        image_1138 = self.boot.read_bytes()
        self.assertTrue(image_1138.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora7-1\n"))
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff t6030")
        # The plain one-liner of this release moves a listed J516S.
        self.install()
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:" + flow.M1N1_BASE.encode() + b"\n"), boot[:80])
        self.assertTrue(boot.endswith(flow.SWITCHES))
        # Each release keeps what the Mac booted before it.
        self.assertEqual(self.kept(self.old_version).read_bytes(), b"M1N1:original\n")
        self.assertEqual(self.kept(VERSION).read_bytes(), image_1138)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {flow.PRO_VARIANT}")
        # It stops; the owner puts 11.38's image back and freezes updates.
        self.boot.write_bytes(self.kept(VERSION).read_bytes())
        with open(self.update_conf, "a") as f:
            f.write("M1N1_UPDATE_DISABLED=1\n")
        proc = self.install()
        self.assertNotIn("Remove that line", proc.stdout + proc.stderr)
        self.assertEqual(self.boot.read_bytes(), image_1138)
        self.assertEqual((self.state / "m1n1-failed").read_text().split()[0], self.bin_shas[flow.M1N1_PKG])

    def test_j514s_opted_in_on_11_38(self):
        self.mac("j514s")
        self.install_1138(try_=1)
        before = self.boot.read_bytes()
        log = self.log()
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("--m3-handoff", proc.stderr)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("curl ", self.log()[len(log):])

    def test_m1_from_11_38(self):
        self.mac("j314s")
        self.install_1138()
        image_1138 = self.boot.read_bytes()
        self.assertTrue(image_1138.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora3-1\n"))
        self.assertFalse(self.kept(self.old_version).exists())
        proc = self.install()
        self.assertIn("put the\n    boot loader it booted with back from macOS", proc.stderr)
        self.assertEqual(self.kept(VERSION).read_bytes(), image_1138)
        self.assertTrue(self.boot.read_bytes().startswith(b"M1N1:" + flow.M1N1_BASE.encode() + b"\n"))
        self.assertNotIn(b"chosen.", self.boot.read_bytes())

    def test_neo_from_11_38(self):
        own = self.tmp / "neo-m1n1.bin"
        own.write_bytes(b"M1N1:neo-own\n")
        self.mac("j700")
        self.update_conf.write_text(f"M1N1={own}\nU_BOOT=/x\n")
        self.install_1138()
        image_1138 = self.boot.read_bytes()
        self.install()
        self.assertEqual(self.boot.read_bytes(), image_1138)
        self.assertFalse(self.kept(VERSION).exists())
        self.assertNotIn("m1n1-aurora", " ".join(self.downloaded()))


if __name__ == "__main__":
    unittest.main()
