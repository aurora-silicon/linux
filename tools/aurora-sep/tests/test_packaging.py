#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The packaging must describe the files in this tree, not stale copies."""

import hashlib
import re
import unittest
from pathlib import Path

SEP = Path(__file__).resolve().parents[1]
ARCH = SEP / "packaging" / "arch"
FEDORA = SEP / "packaging" / "fedora"


def bash_array(text, name):
    match = re.search(r"^%s=\((.*?)\)" % re.escape(name), text, re.M | re.S)
    if match is None:
        raise AssertionError("no %s array" % name)
    return re.findall(r"'([^']*)'|\"([^\"]*)\"|(\S+)", match.group(1))


def words(text, name):
    return ["".join(parts) for parts in bash_array(text, name)]


class PkgbuildTest(unittest.TestCase):
    def check(self, directory):
        text = (directory / "PKGBUILD").read_text()
        sources = words(text, "source")
        sums = words(text, "sha256sums")
        self.assertEqual(len(sources), len(sums))
        pkgver = re.search(r"^pkgver=(\S+)$", text, re.M).group(1)
        for source, expected in zip(sources, sums):
            if "://" in source:
                continue
            name = source.replace("$pkgver", pkgver)
            path = directory / name
            self.assertTrue(path.is_file(), "%s is not a file" % path)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(
                digest, expected, "%s changed: run updpkgsums in %s" % (name, directory)
            )

    def test_aurora_sep_sums_match_tree(self):
        self.check(ARCH / "aurora-sep")


class FedoraSpecTest(unittest.TestCase):
    def test_sources_are_in_tree(self):
        text = (FEDORA / "aurora-sep.spec").read_text()
        sources = re.findall(r"^Source\d+:\s+(\S+)$", text, re.M)
        self.assertTrue(sources)
        for source in sources:
            self.assertTrue((SEP / source).is_file(), source)

    def fc_entries(self):
        return [line.split() for line in (SEP / "aurora_sep.fc").read_text().splitlines()
                if line.strip() and not line.startswith("#")]

    def test_selinux_module_labels_the_loader_device(self):
        (path, kind, context), = [e for e in self.fc_entries() if e[1] == "-c"]
        self.assertIn(path, (SEP / "load-driver").read_text())
        label = re.search(r":(\w+_t),", context).group(1)
        te = (SEP / "aurora_sep.te").read_text()
        self.assertRegex(te, r"(?m)^type %s;$" % label)

    def test_the_loader_runs_in_its_own_domain(self):
        unit = (SEP / "aurora-sep.service").read_text()
        loader = re.search(r"(?m)^ExecStart=(\S+)$", unit).group(1)
        files = {e[0]: e[2] for e in self.fc_entries() if e[1] == "--"}
        for path in (loader, "/usr/libexec/aurora-sep-load"):
            self.assertIn(":aurora_sep_load_exec_t,", files[path], path)
        te = (SEP / "aurora_sep.te").read_text()
        self.assertIn("init_daemon_domain(aurora_sep_load_t, aurora_sep_load_exec_t)", te)
        # Other domains only stop logging the view of the probe keys; none
        # gains access to them.
        self.assertEqual(re.findall(r"(?m)^dontaudit .*$", te),
                         ["dontaudit domain aurora_sep_load_t:key view;"])
        self.assertNotRegex(te, r"allow\s+\S+\s+\S+:key")

    def test_build_script_builds_only_aurora_sep(self):
        text = (FEDORA / "build.sh").read_text()
        self.assertIn("aurora-sep.spec", text)
        self.assertNotIn("dnf -y download --source libfprint", text)


if __name__ == "__main__":
    unittest.main()
