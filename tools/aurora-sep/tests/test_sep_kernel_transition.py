"""The shipped unit must skip old built-in kernels and accept real modules."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

UNIT = Path(__file__).resolve().parents[1]/'aurora-sep.service'


class KernelTransitionTests(unittest.TestCase):
    def test_running_kernel_transition(self):
        condition = next(line.split('=', 1)[1] for line in UNIT.read_text().splitlines()
                         if line.startswith('ExecCondition='))
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory)/'modinfo'
            probe.write_text('#!/bin/sh\nif [ "$MODULE_RESULT" = absent ]; then exit 1; fi\nprintf "%s\\n" "$MODULE_RESULT"\n')
            probe.chmod(0o755)
            for result, accepted in [('absent', False), ('(builtin)', False), ('', False),
                    ('/lib/modules/7.2.10/kernel/sep/apple_sep.ko', True),
                    ('/lib/modules/7.2.10/kernel/sep/apple_sep.ko.zst', True),
                    ('/lib/modules/7.2.10/kernel/sep/apple_sep.ko.xz', True),
                    ('/lib/modules/7.2.10/kernel/sep/apple_sep.ko.gz', True)]:
                with self.subTest(module=result):
                    check = subprocess.run(shlex.split(condition), env={**os.environ,
                        'PATH': directory+':'+os.environ['PATH'], 'MODULE_RESULT': result})
                    self.assertEqual(check.returncode == 0, accepted)
