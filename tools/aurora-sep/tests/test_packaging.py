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

    def test_build_script_builds_only_aurora_sep(self):
        text = (FEDORA / "build.sh").read_text()
        self.assertIn("aurora-sep.spec", text)
        self.assertNotIn("dnf -y download --source libfprint", text)


if __name__ == "__main__":
    unittest.main()
