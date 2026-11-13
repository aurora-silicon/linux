#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check source validation in the DART firmware test harness."""
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest


HARNESS = Path(__file__).with_name('apple-dart-fw-tests.py')
ROOT = HARNESS.resolve().parents[2]


class SourceValidationTests(unittest.TestCase):
    def test_missing_function(self):
        harness = runpy.run_path(str(HARNESS))
        source = (ROOT / 'drivers/iommu/apple-dart.c').read_text()
        name = 'apple_dart_fw_handoff_enabled'
        body = harness['function'](source, name)
        self.assertIsNotNone(body)
        with tempfile.TemporaryDirectory(prefix='dart-fw-harness-') as tmp:
            source_path = Path(tmp) / 'missing.c'
            source_path.write_text(source.replace(body, '', 1))
            # Case selection does not remove other controls from the C fixture.
            for cases in ([], ['--case', 'mirror'],
                          ['--case', 'mirror', '--case', 'retire']):
                with self.subTest(cases=cases):
                    out = Path(tmp) / f'out-{len(cases)}'
                    result = subprocess.run(
                        [sys.executable, '-I', '-B', str(HARNESS),
                         '--source', str(source_path), '--out', str(out),
                         *cases], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn('production functions missing: ' + name,
                                  result.stderr)
                    self.assertNotIn('Traceback', result.stderr)
                    self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
