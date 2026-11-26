# SPDX-License-Identifier: GPL-2.0-only
"""The aurora-touchid package (Omarchy): fprintd gets /dev/sep-bio only, not the whole misc
device class, and starts after the node exists; the PKGBUILD's checksums match its files."""
import configparser
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

PKG = Path(__file__).resolve().parents[1] / 'aurora-touchid'


def unit(path):
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.optionxform = str
    cp.read_string(path.read_text())
    return cp


def words(cp, section, key):
    return [w for line in cp.get(section, key, fallback='').splitlines() for w in line.split()]


class TouchIdPackageTest(unittest.TestCase):
    def test_fprintd_gets_only_the_touch_id_node(self):
        text = (PKG / 'fprintd-aurora.conf').read_text()
        rules = [l.split('=', 1)[1].strip() for l in text.splitlines() if l.startswith('DeviceAllow=')]
        self.assertEqual(rules, ['/dev/sep-bio rw'])
        self.assertNotIn('char-misc', ''.join(rules))
        # No wait inside fprintd's own start: the path rule is resolved before ExecStartPre= runs.
        self.assertFalse(any(l.startswith('ExecStartPre') for l in text.splitlines()))

    def test_fprintd_starts_after_the_node_is_there(self):
        dropin = unit(PKG / 'fprintd-aurora.conf')
        self.assertIn('aurora-touchid-device.service', words(dropin, 'Unit', 'After'))
        self.assertIn('aurora-touchid-device.service', words(dropin, 'Unit', 'Wants'))
        self.assertIn('aurora-sep-ready.service', words(dropin, 'Unit', 'After'))
        wait = unit(PKG / 'aurora-touchid-device.service')
        self.assertIn('fprintd.service', words(wait, 'Unit', 'Before'))
        self.assertIn('apple-sep.service', words(wait, 'Unit', 'After'))
        self.assertEqual(wait.get('Service', 'Type'), 'oneshot')
        self.assertIn('/dev/sep-bio', wait.get('Service', 'ExecStart'))

    def test_the_wait_ends(self):
        cmd = unit(PKG / 'aurora-touchid-device.service').get('Service', 'ExecStart')
        script = re.match(r"/bin/sh -c '(.*)'$", cmd).group(1).replace('/dev/sep-bio', '/nonexistent/sep-bio')
        script = script.replace('-lt 150', '-lt 3')
        self.assertEqual(subprocess.run(['sh', '-c', script], timeout=10).returncode, 0)

    def test_pkgbuild_installs_and_checks_every_source(self):
        text = (PKG / 'PKGBUILD').read_text()
        sources = re.findall(r"'([^']+)'", text.split('source=(', 1)[1].split(')', 1)[0])
        sums = re.findall(r"'([0-9a-f]{64})'", text.split('sha256sums=(', 1)[1].split(')', 1)[0])
        self.assertEqual(len(sources), len(sums))
        for name, digest in zip(sources, sums):
            self.assertEqual(hashlib.sha256((PKG / name).read_bytes()).hexdigest(), digest, name)
        self.assertIn('aurora-touchid-device.service', sources)
        self.assertIn('"$pkgdir/usr/lib/systemd/system/aurora-touchid-device.service"', text)

    @unittest.skipUnless(shutil.which('systemd-analyze'), 'systemd-analyze is needed')
    def test_units_verify(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'fprintd.service.d').mkdir()
            (d / 'fprintd.service').write_text('[Unit]\nDescription=stub\n[Service]\nExecStart=/bin/true\n')
            shutil.copy(PKG / 'fprintd-aurora.conf', d / 'fprintd.service.d/aurora.conf')
            shutil.copy(PKG / 'aurora-touchid-device.service', d)
            r = subprocess.run(['systemd-analyze', 'verify', '--man=no',
                                str(d / 'aurora-touchid-device.service'), str(d / 'fprintd.service')],
                               capture_output=True, text=True, env={'SYSTEMD_UNIT_PATH': f'{d}:', 'PATH': '/usr/bin:/bin'})
            self.assertNotIn('aurora-touchid-device', r.stderr)
            self.assertNotIn('aurora.conf', r.stderr)


if __name__ == '__main__':
    unittest.main()
