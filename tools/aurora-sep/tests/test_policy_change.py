# SPDX-License-Identifier: GPL-2.0-only
"""aurora-sep-policy-change arms its allowance where SELinux isn't installed."""
from pathlib import Path
import importlib.machinery
import importlib.util
import os
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'aurora-sep-policy-change'
LOADER = importlib.machinery.SourceFileLoader('policy_change', str(SCRIPT))
M = importlib.util.module_from_spec(importlib.util.spec_from_loader('policy_change', LOADER))
LOADER.exec_module(M)


class ArmTest(unittest.TestCase):
    def test_arm_without_restorecon(self):
        with tempfile.TemporaryDirectory() as d:
            allow = os.path.join(d, 'allow-once.bin')
            # PATH holds only the empty temporary directory, as on a system without SELinux
            with patch.object(M, 'ALLOW', allow), patch.dict(os.environ, {'PATH': d}):
                M.arm(b'record')
            self.assertEqual(Path(allow).read_bytes(), b'record')
            self.assertFalse(os.path.exists(allow + '.tmp'))


if __name__ == '__main__':
    unittest.main()
