"""Release guards and upgrades from the previous release.

ReleaseGuardTest fails while any package checksum is still a placeholder
(the M3 Pro's Mesa and its dependency list included): a release is cut only
when it passes. RealProMesaPackageTest checks the M3 Pro's Mesa package by
its name, and by its content when the file is at hand (AURORA_PRO_MESA_PKG,
or the release staging directory). RealM1n1PackageTest checks the
m1n1 package this release names, by its file name always, and by its
content when the file is at hand (AURORA_M1N1_PKG, or the release staging
directory). ReleaseUrlTest and StagedCopyTest cover the staging or mirror
override (AURORA_RELEASE_URL, AURORA_RELEASES_API): the default is the
release's own tag, only a plain file://, http:// or https:// URL is taken,
a staged copy is named once at the start and installs through the same
download and checksum loop, and the commands an install prints name the
public release. Only a newer release number is named, and under an override
with no command to run. PublicDownloadTest keeps the public release's own message
for a missing file. UpgradeTest runs 11.38's own script on the fake Mac of
test_m3_flow, then this one, as an owner updating would; Upgrade120Test and
Upgrade121Test do the same from 12.0's and 12.1's.
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
EARLIER_M1N1 = ["1.6.1.aurora3-1", "1.6.1.aurora7-1", "1.6.1.aurora8.4-1", "1.6.1.aurora8.5-2", "1.6.1.aurora12-1", "1.6.1.aurora12.1-2"]


def package_entries():
    """Every "file sha256" this script downloads (PACKAGES and M1N1_PACKAGE)."""
    block = re.search(r"^PACKAGES=\(\n(.*?)^\)", SRC, re.M | re.S).group(1)
    entries = re.findall(r'^\s*"(\S+) (\S+)"$', block, re.M)
    entries.append((M1N1_PACKAGE.group(1), M1N1_PACKAGE.group(2)))
    return entries


def gpu_entries():
    """What --m3-gpu-experiment downloads: M3_GPU_SCRIPTS and M3_GPU_MESA_PACKAGE (when set)."""
    block = re.search(r"^M3_GPU_SCRIPTS=\(\n(.*?)^\)", SRC, re.M | re.S)
    entries = re.findall(r'^\s*"(\S+) (\S+)"$', block.group(1), re.M) if block else []
    mesa = re.search(r'^M3_GPU_MESA_PACKAGE="(\S+) (\S+)"$', SRC, re.M)
    if mesa:
        entries.append((mesa.group(1), mesa.group(2)))
    return entries


def pro_mesa_entries():
    """What an M3 Pro downloads besides: M3_PRO_MESA_PACKAGE (when set)."""
    m = re.search(r'^M3_PRO_MESA_PACKAGE="(\S+) (\S+)"$', SRC, re.M)
    return [(m.group(1), m.group(2))] if m else []


M3_PRO_MESA_NEEDS = re.search(r'^M3_PRO_MESA_NEEDS="([^"]*)"$', SRC, re.M).group(1)
M3_PRO_MESA_DETECTOR = re.search(r'^M3_PRO_MESA_DETECTOR="([^"]*)"$', SRC, re.M).group(1)
M3_PRO_MESA_SETUP_LIST = re.search(r'^M3_PRO_MESA_SETUP_LIST="([^"]*)"$', SRC, re.M).group(1)
M3_PRO_MESA_DETECTOR_SHA256 = re.search(r'^M3_PRO_MESA_DETECTOR_SHA256=(\S*)$', SRC, re.M).group(1)
M3_PRO_MESA_SETUP_LIST_SHA256 = re.search(r'^M3_PRO_MESA_SETUP_LIST_SHA256=(\S*)$', SRC, re.M).group(1)
M3_PRO_MESA_INTEGRATION = re.search(r'^M3_PRO_MESA_INTEGRATION="([^"]*)"$', SRC, re.M).group(1).splitlines()


def builtin_copy(fn, installer=None):
    """What the installer's heredoc function fn prints: its byte copy of a mesa-m3 file."""
    return subprocess.run(["bash", "-c", f"AURORA_SEP_SOURCE_ONLY=1 source '{installer or flow.INSTALLER}'; {fn}"],
                          capture_output=True, check=True).stdout


def staged(name, env):
    candidates = [os.environ.get(env, "")]
    stage = Path.home() / "source/aurora-recipes" / f"stage-{VERSION.split('-')[-1]}"
    candidates.append(str(stage / name))
    # A package built and not yet staged.
    candidates.append(str(Path.home() / "source/aurora-recipes/builds" / name.removesuffix("-aarch64.pkg.tar.zst") / name))
    for c in candidates:
        if c and Path(c).is_file() and Path(c).name == name:
            return Path(c)
    return None


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
        pending = [f"{f} {sha}" for f, sha in package_entries() + gpu_entries() + pro_mesa_entries()
                   if not re.fullmatch(r"[0-9a-f]{64}", sha) or "PENDING" in f]
        if not re.fullmatch(r"[0-9a-f]{64}", M1N1_BIN_SHA):
            pending.append(f"M1N1_BIN_SHA={M1N1_BIN_SHA}")
        for name, value in (("M3_PRO_MESA_NEEDS", M3_PRO_MESA_NEEDS), ("M3_PRO_MESA_DETECTOR", M3_PRO_MESA_DETECTOR),
                            ("M3_PRO_MESA_SETUP_LIST", M3_PRO_MESA_SETUP_LIST)):
            if "PENDING" in value:
                pending.append(f"{name}={value}")
        for name, value in (("M3_PRO_MESA_DETECTOR_SHA256", M3_PRO_MESA_DETECTOR_SHA256),
                            ("M3_PRO_MESA_SETUP_LIST_SHA256", M3_PRO_MESA_SETUP_LIST_SHA256)):
            if not re.fullmatch(r"[0-9a-f]{64}", value):
                pending.append(f"{name}={value}")
        self.assertEqual(pending, [], "placeholders left in install-aurora-sep.sh")

    def test_packages_follow_version(self):
        names = [f for f, _ in package_entries()]
        self.assertIn(f"linux-aurora-$VERSION-aarch64.pkg.tar.zst", names)
        self.assertIn(f"linux-aurora-headers-$VERSION-aarch64.pkg.tar.zst", names)
        self.assertEqual(len([n for n in names if n.startswith("m1n1-")]), 1, names)


FILL = Path.home() / "source/aurora-recipes/tests/fill-m3-pro-mesa.sh"


@unittest.skipUnless(FILL.is_file() and shutil.which("bsdtar"), "fill-m3-pro-mesa.sh and bsdtar are needed")
class FillProMesaTest(unittest.TestCase):
    """fill-m3-pro-mesa.sh on a copy of this script: it fills the package, NEEDS, the detector
    paths and their sha256, and refuses a package whose detector or list is not byte for byte
    the installer's copy (unless --embed, which writes the package's bytes into the copy), or
    that lacks one of M3_PRO_MESA_INTEGRATION."""

    DETECTOR = "/opt/mesa-m3/libexec/mesa-m3-user-setup"
    LIST = "/opt/mesa-m3/share/mesa-m3/user-setup.list"

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.inst = self.tmp / "install-aurora-sep.sh"
        self.inst.write_text(SRC)
        self.detector = builtin_copy("m3_pro_mesa_builtin_detector")
        self.listed = builtin_copy("m3_pro_mesa_builtin_list")

    def package(self, detector=None, listed=None, leave_out=()):
        root = self.tmp / "root"
        shutil.rmtree(root, ignore_errors=True)
        files = {self.DETECTOR: detector or self.detector, self.LIST: listed or self.listed}
        files.update({p: b"x\n" for p in M3_PRO_MESA_INTEGRATION if p not in leave_out})
        for p, data in files.items():
            (root / p.lstrip("/")).parent.mkdir(parents=True, exist_ok=True)
            (root / p.lstrip("/")).write_bytes(data)
        (root / ".PKGINFO").write_text("pkgname = mesa-m3\npkgver = 26.1.4.m3.1-9\narch = aarch64\n"
                                        "depend = glibc>=2.43\n")
        pkg = self.tmp / "mesa-m3-26.1.4.m3.1-9-aarch64.pkg.tar.zst"
        subprocess.run(["bsdtar", "--zstd", "-cf", str(pkg), "-C", str(root), ".PKGINFO", "opt", "usr"], check=True)
        return pkg

    def fill(self, pkg, *opts):
        return subprocess.run(["bash", str(FILL), *opts, str(self.inst), str(pkg), "glibc>=2.43", self.DETECTOR,
                               self.LIST], capture_output=True, text=True)

    def test_the_same_bytes_fill(self):
        proc = self.fill(self.package())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        filled = self.inst.read_text()
        self.assertIn(f'M3_PRO_MESA_DETECTOR="{self.DETECTOR}"', filled)
        self.assertIn(f"M3_PRO_MESA_DETECTOR_SHA256={hashlib.sha256(self.detector).hexdigest()}\n", filled)
        self.assertIn(f"M3_PRO_MESA_SETUP_LIST_SHA256={hashlib.sha256(self.listed).hexdigest()}\n", filled)
        self.assertIn('M3_PRO_MESA_PACKAGE="mesa-m3-26.1.4.m3.1-9-aarch64.pkg.tar.zst ', filled)

    def test_other_bytes_are_refused_unless_embedded(self):
        listed = self.listed + b"variable\tMESA_EXTRA\n"
        detector = self.detector.replace(b"PREFIX = '/opt/mesa-m3'", b"PREFIX = '/opt/mesa-m3'  # changed")
        self.assertNotEqual(detector, self.detector)
        for name, kw, why in (("list", {"listed": listed}, "user-setup list differs"),
                              ("detector", {"detector": detector}, "copy of the detector is not")):
            with self.subTest(name):
                proc = self.fill(self.package(**kw))
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(why, proc.stderr)
                self.assertEqual(self.inst.read_text(), SRC)
        # --embed writes the package's bytes into the copies, and then fills.
        proc = self.fill(self.package(detector=detector, listed=listed), "--embed")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(builtin_copy("m3_pro_mesa_builtin_detector", self.inst), detector)
        self.assertEqual(builtin_copy("m3_pro_mesa_builtin_list", self.inst), listed)
        self.assertIn(f"M3_PRO_MESA_DETECTOR_SHA256={hashlib.sha256(detector).hexdigest()}\n", self.inst.read_text())
        self.assertEqual(subprocess.run(["bash", "-n", str(self.inst)]).returncode, 0)

    def test_a_package_without_an_integration_file_is_refused(self):
        proc = self.fill(self.package(leave_out=(M3_PRO_MESA_INTEGRATION[1],)))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(f"has no {M3_PRO_MESA_INTEGRATION[1].lstrip('/')} (M3_PRO_MESA_INTEGRATION)", proc.stderr)
        self.assertEqual(self.inst.read_text(), SRC)

    # The package's own detector against the built-in copy: test_m3_pro_mesa.DetectorAgreementTest.


class RealProMesaPackageTest(unittest.TestCase):
    """The M3 Pro's Mesa package this release names: by its file name always, and by its content
    when the file is at hand (AURORA_PRO_MESA_PKG, or the release staging directory)."""

    # Where the package may put files: its prefix, its licence, and the files that tie it into the
    # system (M3_PRO_MESA_INTEGRATION: the uwsm hook, the render node's udev rule, the user unit
    # and its wants link), which the record names.
    ALLOWED = ("opt/mesa-m3/", "usr/share/licenses/mesa-m3/") + tuple(p.lstrip("/") for p in M3_PRO_MESA_INTEGRATION)
    # The install scriptlet's lines: comments, function bodies that only print, calls of those
    # functions, and the one read-only test of whether group render has members.
    SCRIPTLET = (r'^$|^#|^[a-z_]+\(\) \{$|^\}$|^fi$|^echo "[^"`$]*(\\\$[^"`$]*)*"$|^_[a-z_]+$'
                 r'|^if \[ -z "\$\(getent group render 2>/dev/null \| cut -d: -f4\)" \]; then$')

    def setUp(self):
        if not pro_mesa_entries():
            self.skipTest("this release names no M3 Pro Mesa package")
        self.name, self.sha = pro_mesa_entries()[0]

    def test_name(self):
        if "PENDING" in self.name:
            self.skipTest("a placeholder: ReleaseGuardTest fails on it")
        self.assertRegex(self.name, r"^mesa-m3-[A-Za-z0-9._+:]+-[0-9.]+-aarch64\.pkg\.tar\.zst$")
        self.assertNotIn("PENDING", self.name)

    def test_the_package_itself(self):
        path = staged(self.name, "AURORA_PRO_MESA_PKG")
        if path is None or "PENDING" in self.name:
            self.skipTest(f"{self.name} is not at hand (set AURORA_PRO_MESA_PKG)")
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), self.sha)
        listing = subprocess.run(["bsdtar", "-tf", str(path)], capture_output=True, text=True, check=True).stdout
        info = subprocess.run(["bsdtar", "-xOf", str(path), ".PKGINFO"], capture_output=True, text=True,
                              check=True).stdout
        self.assertEqual(re.search(r"^pkgname = (\S+)$", info, re.M).group(1), "mesa-m3")
        pkgver = re.search(r"^pkgver = (\S+)$", info, re.M).group(1)
        self.assertEqual(self.name, f"mesa-m3-{pkgver}-aarch64.pkg.tar.zst")
        files = [f for f in listing.splitlines() if f and not f.startswith(".") and not f.endswith("/")]
        self.assertEqual([f for f in files if not f.startswith(self.ALLOWED)], [])
        # An install scriptlet may only print: everything the package does is in its files, so
        # pacman -R mesa-m3 undoes it all.
        if ".INSTALL" in listing.splitlines():
            script = subprocess.run(["bsdtar", "-xOf", str(path), ".INSTALL"], capture_output=True, text=True,
                                    check=True).stdout
            for line in script.splitlines():
                with self.subTest(line=line):
                    self.assertRegex(line.strip(), self.SCRIPTLET)
        # Never over the system Mesa: it provides nothing, and conflicts with and replaces only
        # the earlier experiment-only mesa-m3-g15g (12.4 on), which the installer removes first.
        self.assertNotRegex(info, r"(?m)^provides = ")
        for key in ("conflict", "replaces"):
            self.assertEqual(re.findall(rf"(?m)^{key} = (.*)$", info), ["mesa-m3-g15g"], key)
        # mesa-m3's user-setup detector and its list, the list the same as the built-in one.
        self.assertIn(M3_PRO_MESA_DETECTOR.lstrip("/"), files)
        self.assertIn(M3_PRO_MESA_SETUP_LIST.lstrip("/"), files)
        for p in M3_PRO_MESA_INTEGRATION:
            self.assertIn(p.lstrip("/"), files)
        # The installer's byte copies are the package's files, with the sha256 it names.
        for p, fn, sha in ((M3_PRO_MESA_DETECTOR, "m3_pro_mesa_builtin_detector", M3_PRO_MESA_DETECTOR_SHA256),
                           (M3_PRO_MESA_SETUP_LIST, "m3_pro_mesa_builtin_list", M3_PRO_MESA_SETUP_LIST_SHA256)):
            data = subprocess.run(["bsdtar", "-xOf", str(path), p.lstrip("/")], capture_output=True, check=True).stdout
            self.assertEqual(hashlib.sha256(data).hexdigest(), sha, p)
            self.assertEqual(builtin_copy(fn), data, fn)
        # Every dependency is in M3_PRO_MESA_NEEDS, at the package's minimum or above, so the
        # installer's check covers everything its pacman -U could otherwise pull in.
        needs = {}
        for n in M3_PRO_MESA_NEEDS.split():
            name, _, minimum = n.partition(">=")
            needs[name] = minimum
        for dep in re.findall(r"^depend = (\S+)$", info, re.M):
            name, op, minimum = re.match(r"([^<>=]+)(>=|=|>|<=|<)?(.*)", dep).groups()
            with self.subTest(dep=dep):
                self.assertIn(name, needs)
                if minimum and shutil.which("vercmp"):
                    self.assertTrue(needs[name], f"{name} needs a minimum in M3_PRO_MESA_NEEDS")
                    out = subprocess.run(["vercmp", needs[name], minimum], capture_output=True, text=True).stdout
                    self.assertIn(out.strip(), ("0", "1"))


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
LATEST_URL = re.search(r"^LATEST_URL=(\S+)$", SRC, re.M).group(1)
# sep-<kernel version>-<release>: the release number orders releases, whatever
# the kernel version.
_kernel, _major, _minor = re.match(r"sep-([^-]+)-(\d+)\.(\d+)", TAG).groups()
_major, _minor = int(_major), int(_minor)
NEWER_TAGS = (f"{TAG}.1", f"sep-{_kernel}-{_major}.{_minor + 1}", f"sep-{_kernel}-{_major + 1}.0",
              f"sep-1.0.0.aurora1-{_major + 1}.0")
OLDER_TAGS = (TAG, f"sep-{_kernel}-{_major - 1}.38", f"sep-{_kernel}-{_major - 1}.36.1",
              f"sep-{_kernel}-{_major - 1}.9", f"sep-99.0.0.aurora9-{_major - 1}.99", "sep-latest")
OVERRIDES = ("AURORA_RELEASE_URL", "AURORA_RELEASES_API")
# Values release_source refuses: curl options, no or another scheme, spaces,
# control characters and bytes outside printable ASCII.
BAD_VALUES = ("-K/etc/hostname", "--config=/tmp/x", "-o/tmp/x", "ftp://mirror/x", "stage-12.0",
              "/srv/stage", "file:///srv/st age", "https://m/a\tb", "https://m/a\nb",
              "https://m/\x1b]0;t\x07", "https://m/\x1b[2J", "https://m/\x7f", "https://m/\u00e9",
              "HTTPS://m/x", "file://")


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

    def test_bad_values_are_refused(self):
        for name in OVERRIDES:
            for value in BAD_VALUES:
                with self.subTest(name=name, value=value):
                    proc = sourced("release_source", **{name: value})
                    self.assertEqual(proc.returncode, 1, proc.stdout)
                    schemes = "a file://, http:// or https:// URL"
                    self.assertIn(f"{name} must be {schemes}", proc.stderr)
                    # The value itself is never echoed.
                    self.assertNotIn(value, proc.stderr.replace(schemes, ""))

    def test_good_values_are_taken(self):
        for value in ("file:///srv/stage", "file:///srv/stage/", "file://localhost/srv/a%20b",
                      "http://127.0.0.1:18712", "https://mirror.example/aurora/12.0/"):
            with self.subTest(value=value):
                proc = sourced("release_source", AURORA_RELEASE_URL=value, AURORA_RELEASES_API=value)
                self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_the_copy_in_use_is_named_once(self):
        notice = "Using a staging/mirror copy: %s; checksums are still verified"
        for env, shown in (({}, None),
                           ({"AURORA_RELEASE_URL": DEFAULT_RELEASE_URL}, None),
                           ({"AURORA_RELEASE_URL": "file:///srv/stage"}, "file:///srv/stage"),
                           ({"AURORA_RELEASES_API": "http://127.0.0.1:8000/api"},
                            "release list http://127.0.0.1:8000/api"),
                           ({"AURORA_RELEASE_URL": "file:///srv/stage", "AURORA_RELEASES_API": "http://127.0.0.1:8000/api"},
                            "file:///srv/stage (release list http://127.0.0.1:8000/api)")):
            with self.subTest(env=env):
                proc = sourced("release_source", **env)
                self.assertEqual(proc.returncode, 0, proc.stderr)
                if shown is None:
                    self.assertEqual(proc.stdout, "")
                else:
                    self.assertEqual(len(proc.stdout.splitlines()), 1, proc.stdout)
                    self.assertIn(notice % shown, proc.stdout)

    def test_agent_prompt_names_the_copy_on_stderr(self):
        if not shutil.which("curl"):
            self.skipTest("curl is needed")
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "latest").write_text('{"tag_name": "%s"}' % TAG)
            env = {k: v for k, v in os.environ.items() if k not in OVERRIDES}
            env.update(AURORA_RELEASE_URL="file:///srv/stage", AURORA_RELEASES_API=Path(d).as_uri())
            proc = subprocess.run(["bash", str(flow.INSTALLER), "--agent-prompt"],
                                  capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Using a staging/mirror copy: file:///srv/stage", proc.stderr)
        # The prompt itself is untouched and names only the public release.
        self.assertNotIn("staging/mirror", proc.stdout)
        self.assertNotIn("file:///srv/stage", proc.stdout)
        self.assertNotIn(Path(d).as_uri(), proc.stdout)

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
            # A tag with control characters or anything but [A-Za-z0-9._-] is not taken.
            for tag in (b"sep-7.1.12.aurora2-99.0\x1b]0;x\x07", b"sep-7.1.12.aurora2-99.0\x1b[2J",
                        b"sep-7.1.12.aurora2-99.0\\u001b[2J", b"sep-99.0$(id)", b"sep-99.0 x"):
                with self.subTest(tag=tag):
                    latest.write_bytes(b'{"tag_name": "' + tag + b'"}')
                    proc = sourced("newer_release", AURORA_RELEASES_API=api)
                    self.assertEqual((proc.returncode, proc.stdout), (0, ""), proc.stderr)

    def api(self, d, tag):
        (Path(d) / "latest").write_text('{"tag_name": "%s"}' % tag)
        return Path(d).as_uri()

    def test_only_a_newer_release_is_named(self):
        if not shutil.which("curl"):
            self.skipTest("curl is needed")
        with tempfile.TemporaryDirectory() as d:
            for tag, newer in [(t, True) for t in NEWER_TAGS] + [(t, False) for t in OLDER_TAGS]:
                with self.subTest(tag=tag):
                    proc = sourced("newer_release", AURORA_RELEASES_API=self.api(d, tag))
                    self.assertEqual(proc.stdout.strip(), tag if newer else "", proc.stderr)

    def notice(self, d, tag, override):
        api = self.api(d, tag)
        if override:
            return sourced("version_notice", AURORA_RELEASES_API=api)
        # The public check, read from the same file.
        return sourced(f"RELEASES_API={api}; PUBLIC_RELEASES_API={api}; version_notice")

    def test_version_notice(self):
        if not shutil.which("curl"):
            self.skipTest("curl is needed")
        newer, older = NEWER_TAGS[-2], OLDER_TAGS[1]
        with tempfile.TemporaryDirectory() as d:
            # The public script: a newer release points at the current script.
            proc = self.notice(d, newer, override=False)
            self.assertIn(f"this script installs {TAG}, but {newer} is published", proc.stderr)
            self.assertIn(f"curl -fsSL {LATEST_URL} | bash", proc.stderr)
            # An older or the same one is not named.
            for tag in (older, TAG):
                proc = self.notice(d, tag, override=False)
                self.assertEqual(proc.stderr, "")
                self.assertIn(f"{TAG} is the current release", proc.stdout)
            # An override: a newer release is named, with no command to run.
            proc = self.notice(d, newer, override=True)
            self.assertIn(f"{newer} is newer than {TAG}, which this script installs", proc.stderr)
            self.assertNotIn("curl", proc.stderr)
            self.assertNotIn("saved copy", proc.stderr)
            self.assertNotIn("current release", proc.stdout)
            # An older one: nothing at all.
            proc = self.notice(d, older, override=True)
            self.assertEqual((proc.stdout, proc.stderr), ("", ""))

    def agent_prompt(self, **env_extra):
        env = {k: v for k, v in os.environ.items() if k not in OVERRIDES}
        env.update(env_extra)
        return subprocess.run(["bash", str(flow.INSTALLER), "--agent-prompt"],
                              capture_output=True, text=True, env=env)

    def test_agent_prompt_notice(self):
        if not shutil.which("curl"):
            self.skipTest("curl is needed")
        newer, older = NEWER_TAGS[-2], OLDER_TAGS[1]
        with tempfile.TemporaryDirectory() as d:
            proc = self.agent_prompt(AURORA_RELEASES_API=self.api(d, newer))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn(f"{newer} is newer than {TAG}, which this plan is for", proc.stderr)
            self.assertNotIn("current release", proc.stderr)
            proc = self.agent_prompt(AURORA_RELEASE_URL="file:///srv/stage", AURORA_RELEASES_API=self.api(d, older))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertNotIn(older, proc.stdout + proc.stderr)
            self.assertNotIn("newer", proc.stderr)

    def test_agent_prompt_refuses_a_bad_value(self):
        env = {k: v for k, v in os.environ.items() if k not in OVERRIDES}
        env["AURORA_RELEASES_API"] = "-K/etc/hostname"
        proc = subprocess.run(["bash", str(flow.INSTALLER), "--agent-prompt"],
                              capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("AURORA_RELEASES_API must be", proc.stderr)
        self.assertEqual(proc.stdout, "")


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
        # The stage is named once, in the first line, and nowhere else.
        notice = f"Using a staging/mirror copy: {self.stage.as_uri()}; checksums are still verified"
        self.assertIn(notice, proc.stdout.splitlines()[0])
        self.assertEqual([l for l in printed.splitlines() if self.stage.as_uri() in l],
                         [proc.stdout.splitlines()[0]])
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

    def test_a_bad_value_stops_the_install(self):
        self.mac("j293")
        self.extra_env["AURORA_RELEASE_URL"] = "-K/etc/hostname"
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("AURORA_RELEASE_URL must be", proc.stderr)
        self.assertNotIn("Downloading", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), b"M1N1:original\n")
        self.assertNotIn("pacman -U", self.log())

    def test_a_file_missing_from_the_staged_copy(self):
        self.mac("j293")
        (self.stage / "aurora-touchid-20261003-1-any.pkg.tar.zst").unlink()
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("could not download aurora-touchid-20261003-1-any.pkg.tar.zst", proc.stderr)
        # It blames the staged copy, not the release.
        self.assertIn("from the staging/mirror copy that\n    AURORA_RELEASE_URL names", proc.stderr)
        self.assertNotIn("packaging", proc.stderr)
        self.assertNotIn("report", proc.stderr)
        self.assertNotIn("pacman -U", self.log())


class PublicDownloadTest(flow.M3FlowBase):
    """A file missing from the public release, with no override set."""

    def setUp(self):
        super().setUp()
        self.extra_env.update({name: "" for name in OVERRIDES})

    def test_a_file_missing_from_the_release(self):
        self.mac("j293")
        (self.tmp / "pkgs/aurora-touchid-20261003-1-any.pkg.tar.zst").unlink()
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("could not download aurora-touchid-20261003-1-any.pkg.tar.zst from " + TAG, proc.stderr)
        self.assertIn("packaging\n    mistake", proc.stderr)
        self.assertNotIn("staging/mirror", proc.stdout + proc.stderr)
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


class Upgrade120Test(flow.M3FlowBase):
    """12.0's own script, then this one: a Mac on aurora12 moves to this release's m1n1 as it
    would have moved within 12.0, with nothing asked again."""

    OLD_REV = "df0c4330"

    def setUp(self):
        super().setUp()
        old = subprocess.run(["git", "show", f"{self.OLD_REV}:tools/aurora-sep/install-aurora-sep.sh"],
                             cwd=flow.INSTALLER.parent, capture_output=True)
        if old.returncode:
            self.skipTest(f"git can't show the earlier script ({self.OLD_REV})")
        self.old = self.tmp / f"install-{self.OLD_REV}.sh"
        self.old.write_bytes(old.stdout)
        self.old_version = re.search(rb"^VERSION=(\S+)$", old.stdout, re.M).group(1).decode()
        # A stand-in for 12.0's one m1n1, aurora12, with every switch name.
        self.aurora12 = "m1n1-aurora-1.6.1.aurora12-1-aarch64.pkg.tar.zst"
        self.assertNotEqual(self.aurora12, flow.M1N1_PKG)
        self.fixture(self.aurora12, None, flow.SWITCH_NAMES)

    def install_120(self, try_=0):
        self.installer, self.m1n1_pkg = self.old, self.aurora12
        try:
            return self.run_sh(f"M3_TRY={try_}\ninstall_all")
        finally:
            self.installer, self.m1n1_pkg = flow.INSTALLER, flow.M1N1_PKG

    def kept(self, version):
        return self.boot.parent / f"boot.bin.before-{version}"

    def chosen(self):
        return [l for l in self.m1n1_conf.read_text().splitlines() if l.startswith("chosen.")]

    def assert_moved(self, image_120):
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:" + flow.M1N1_BASE.encode() + b"\n"), boot[:80])
        self.assertEqual(self.kept(VERSION).read_bytes(), image_120)
        self.assertEqual((self.state / "m1n1-installed").read_text().split()[0], self.bin_shas[flow.M1N1_PKG])
        return boot

    def test_j516s_from_12_0(self):
        self.mac("j516s")
        self.install_120()
        image_120 = self.boot.read_bytes()
        self.assertTrue(image_120.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora12-1\n"))
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff t6030-12")
        self.install()
        boot = self.assert_moved(image_120)
        self.assertTrue(boot.endswith(flow.SWITCHES))
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff t6030-12")

    def test_j514s_opted_in_on_12_0(self):
        # Unlike a J514S on 11.38's aurora7: aurora12.1 boots a Pro as aurora12 does, so the
        # opt-in carries over.
        self.mac("j514s")
        self.install_120(try_=1)
        image_120 = self.boot.read_bytes()
        proc = self.install()
        self.assertNotIn("Nothing was installed", proc.stdout + proc.stderr)
        boot = self.assert_moved(image_120)
        self.assertTrue(boot.endswith(flow.SWITCHES))
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff t6030-12")

    def test_air_opted_in_on_12_0(self):
        self.mac("j613")
        self.install_120(try_=1)
        image_120 = self.boot.read_bytes()
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-display-handoff-12")
        switches_120 = self.chosen()
        proc = self.install()
        self.assertNotIn("Run this again with --m3-handoff", proc.stderr)
        self.assert_moved(image_120)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff air-display-handoff-12")
        # The same switches: none of them starts the GPU or its stand-in.
        self.assertEqual(self.chosen(), switches_120)
        self.assertNotIn("chosen.asahi,t8122-gpu=1", self.chosen())
        self.assertNotIn("chosen.asahi,t8122-gpu-power-standin=1", self.chosen())

    def test_air_kernel_only_on_12_0(self):
        # The J615 stays kernel-only; a J613 moves to the display handoff (test_m3_air_default).
        self.mac("j615")
        self.install_120()
        image_120 = self.boot.read_bytes()
        self.install()
        self.assertEqual(self.boot.read_bytes(), image_120)
        self.assertNotIn("m1n1-aurora", " ".join(self.downloaded()))

    def test_m1_from_12_0(self):
        self.mac("j314s")
        self.install_120()
        image_120 = self.boot.read_bytes()
        self.assertTrue(image_120.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora12-1\n"))
        self.install()
        boot = self.assert_moved(image_120)
        self.assertNotIn(b"chosen.", boot)

    def test_m1_whose_aurora12_failed(self):
        # The restore steps of 12.0: the old boot.bin back, updates frozen, aurora12 recorded.
        # The record names aurora12's bytes, not aurora12.1's; the freeze keeps boot.bin.
        self.mac("j314s")
        self.install_120()
        restored = self.kept(self.old_version).read_bytes()
        self.boot.write_bytes(restored)
        with open(self.update_conf, "a") as f:
            f.write("M1N1_UPDATE_DISABLED=1\n")
        (self.state / "m1n1-failed").write_text(f"{self.bin_shas[self.aurora12]} aurora12\n")
        proc = self.install()
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertIn("sets M1N1_UPDATE_DISABLED, so m1n1's boot.bin was not rebuilt", proc.stderr)

    def test_m3_pro_whose_aurora12_failed(self):
        # On an M3 any recorded failure keeps a plain run kernel-only.
        self.mac("j516s")
        self.install_120()
        restored = self.kept(self.old_version).read_bytes()
        self.boot.write_bytes(restored)
        with open(self.update_conf, "a") as f:
            f.write("M1N1_UPDATE_DISABLED=1\n")
        (self.state / "m1n1-failed").write_text(f"{self.bin_shas[self.aurora12]} aurora12\n")
        proc = self.install()
        self.assertIn("an m1n1 from this script failed on this Mac before", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), restored)
        self.assertNotIn(flow.M1N1_PKG, self.downloaded())


class Upgrade121Test(Upgrade120Test):
    """12.1's own script (the same aurora12 and switches as 12.0), then this one."""

    OLD_REV = "1d41e1b7"


if __name__ == "__main__":
    unittest.main()
