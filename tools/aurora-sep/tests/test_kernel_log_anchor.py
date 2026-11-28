# SPDX-License-Identifier: GPL-2.0-only
"""aurora-sep-policy-change and aurora-sep-ready act only on the apple_sep driver's own kernel
messages: a line that merely contains the driver's words (a USB product string, another device)
is not one."""
from pathlib import Path
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
UUID = '0123456789abcdef0123456789abcdef'
OTHER = 'fedcba9876543210fedcba9876543210'
DEVICE = '406600000.sep'


def load_policy_change():
    loader = importlib.machinery.SourceFileLoader('policy_change_anchor',
                                                  str(TOOLS / 'aurora-sep-policy-change'))
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(module)
    return module


M = load_policy_change()


def kernel(msg, device=None, subsystem='platform', transport='kernel', facility='0'):
    e = {'MESSAGE': msg, '_TRANSPORT': transport}
    if facility is not None:
        e['SYSLOG_FACILITY'] = facility
    if device is not None:
        e['_KERNEL_DEVICE'] = device
        e['_KERNEL_SUBSYSTEM'] = subsystem
    return e


REAL_REFUSAL = kernel(f'apple_sep: xART guard: refused to rewrite kind-4 record {UUID}, '
                      'which this Linux install did not create')
REAL_BASE = kernel(f'apple_sep {DEVICE}: xART: APFS-resolved .gl extent (base 0x4d4000); 170 slots, '
                   '10 live records, max revision 1765, 0 malformed, 0 duplicate, 0 repaired; writes ENABLED',
                   f'+platform:{DEVICE}')
FORGED = [
    # A USB device's product string.
    kernel(f'usb 1-1: Product: xART guard: refused to rewrite kind-3 record {OTHER},', '+usb:1-1', 'usb'),
    kernel('usb 1-1: Product: xART: APFS-resolved .gl extent (base 0x1000);', '+usb:1-1', 'usb'),
    # Text that contains the driver's prefix somewhere else, without a device.
    kernel(f'EXT4-fs: bad option apple_sep: xART guard: refused to rewrite kind-4 record {OTHER},'),
    # The device prefix on another subsystem's or device's entry.
    kernel(f'apple_sep {DEVICE}: xART: APFS-resolved .gl extent (base 0x2000);', f'+usb:{DEVICE}', 'usb'),
    kernel('apple_sep 1-1: xART: APFS-resolved .gl extent (base 0x3000);', '+platform:1-1'),
    # A _KERNEL_DEVICE that is not a string.
    dict(kernel(f'apple_sep {DEVICE}: xART: APFS-resolved .gl extent (base 0x5000);'), _KERNEL_DEVICE=[1, 2],
         _KERNEL_SUBSYSTEM='platform'),
    {'MESSAGE': f'apple_sep {DEVICE}: xART: APFS-resolved .gl extent (base 0x6000);', '_TRANSPORT': 'kernel',
     'SYSLOG_FACILITY': '0', '_KERNEL_DEVICE': 7, '_KERNEL_SUBSYSTEM': 'platform'},
    # The kernel transport, but not the kernel's own facility (or none, or not a string).
    kernel(f'apple_sep: xART guard: refused to rewrite kind-4 record {OTHER},', facility='1'),
    kernel(f'apple_sep: xART guard: refused to rewrite kind-3 record {OTHER},', facility='3'),
    kernel(f'apple_sep: xART guard: refused to rewrite kind-4 record {OTHER},', facility=None),
    kernel(f'apple_sep: xART guard: refused to rewrite kind-4 record {OTHER},', facility=0),
    kernel('apple_sep: xART: APFS-resolved .gl extent (base 0x7000);', facility='1'),
    # Not from the kernel.
    kernel(f'apple_sep: xART guard: refused to rewrite kind-4 record {OTHER},', transport='syslog'),
]


class PolicyChangeLog(unittest.TestCase):
    def log(self, entries):
        out = '\n'.join(json.dumps(e) for e in entries) + '\n'
        done = subprocess.CompletedProcess([], 0, stdout=out, stderr='')
        with patch.object(M.subprocess, 'run', return_value=done) as run:
            text = M.kernel_log()
        self.assertIn('json', run.call_args[0][0])
        return text

    def test_forged_lines_are_not_the_drivers(self):
        text = self.log(FORGED)
        self.assertEqual(M.REFUSED.findall(text), [])
        self.assertIsNone(M.BASE.search(text))

    def test_the_drivers_lines_are_read(self):
        text = self.log(FORGED[:2] + [REAL_REFUSAL, REAL_BASE] + FORGED[2:])
        self.assertEqual(M.REFUSED.findall(text), [('4', UUID)])
        self.assertEqual(M.BASE.search(text).group(1), '0x4d4000')

    def test_patterns_are_anchored(self):
        self.assertIsNone(M.REFUSED.search(f'Product: xART guard: refused to rewrite kind-4 record {UUID},'))
        self.assertIsNone(M.BASE.search('Product: xART: APFS-resolved .gl extent (base 0x1);'))

    def test_one_forged_refusal_does_not_block_or_steer_main(self):
        # A forged line next to no real refusal: nothing to allow.
        text = self.log(FORGED + [REAL_BASE])
        with patch.object(M, 'kernel_log', return_value=text), \
                patch.object(M.os, 'geteuid', return_value=0), \
                patch.object(M, 'backup') as backup, patch.object(M, 'arm') as arm:
            with self.assertRaises(SystemExit):
                M.main(['aurora-sep-policy-change'])
        backup.assert_not_called()
        arm.assert_not_called()
        # Next to a real refusal: the real record, not "more than one".
        text = self.log(FORGED + [REAL_REFUSAL, REAL_BASE])
        with patch.object(M, 'kernel_log', return_value=text), \
                patch.object(M.os, 'geteuid', return_value=0), \
                patch.object(M, 'backup', return_value='/x') as backup, \
                patch.object(M, 'arm') as arm:
            M.main(['aurora-sep-policy-change'])
        arm.assert_called_once_with(M.entry(4, bytes.fromhex(UUID)))

    def test_arch_copy_is_the_same(self):
        for name in ('aurora-sep-policy-change', 'aurora-sep-ready'):
            self.assertEqual((TOOLS / 'packaging/arch/aurora-sep' / name).read_bytes(),
                             (TOOLS / name).read_bytes(), name)


class ReadyLog(unittest.TestCase):
    """aurora-sep-ready's rebuild trigger."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bin, self.run = self.root / 'bin', self.root / 'run'
        self.bin.mkdir()
        self.diag = self.root / 'devices' / DEVICE / 'diag'
        self.diag.mkdir(parents=True)
        self.calls = self.root / 'calls'
        for tool in ('journalctl', 'systemctl', 'authselect'):
            (self.bin / tool).write_text(f'#!/bin/sh\necho "{tool} $*" >>"{self.calls}"\n'
                                         + ('cat "$KMSG"\n' if tool == 'journalctl' else ''))
            (self.bin / tool).chmod(0o755)
        self.kmsg = self.root / 'kmsg'

    def ready(self, kmsg):
        for name, value in {'touchid': 'ready', 'sensor_attach': 'not-ready', 'restore': 'restored',
                            'matching': 'usable', 'sensor': 'online'}.items():
            (self.diag / name).write_text(value + '\n')
        self.kmsg.write_text(kmsg)
        done = subprocess.run(['sh', str(TOOLS / 'aurora-sep-ready')], capture_output=True, text=True, env={
            **os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}', 'KMSG': str(self.kmsg),
            'AURORA_SEP_DEV': '/dev/null', 'AURORA_SEP_SYSFS': str(self.root / 'devices'),
            'AURORA_SEP_READY_WAIT': '0', 'AURORA_SEP_RUN': str(self.run)})
        calls = self.calls.read_text() if self.calls.exists() else ''
        self.calls.unlink(missing_ok=True)
        return calls

    def test_reads_only_the_sep_devices_entries(self):
        calls = self.ready('')
        self.assertIn(f'_KERNEL_DEVICE=+platform:{DEVICE}', calls)
        self.assertIn('_KERNEL_SUBSYSTEM=platform', calls)

    def test_a_refusal_in_another_line_starts_nothing(self):
        for line in ('usb 1-1: Product: sensor: LOAD_CALIBRATION refused with status 0x1c',
                     f'usb 1-1: Product: apple_sep {DEVICE}: sensor: SENSOR_ROOT_KEY refused',
                     'apple_sep 1-1: sensor: LOAD_CALIBRATION refused with status 0x1c',
                     'apple_sep 406600000xsep: sensor: LOAD_CALIBRATION refused with status 0x1c'):
            with self.subTest(line):
                calls = self.ready(line + '\n')
                self.assertNotIn('systemctl', calls)
                self.assertFalse((self.run / 'calibration-refused').exists())

    def test_the_drivers_refusal_starts_the_rebuild(self):
        calls = self.ready(f'apple_sep {DEVICE}: sensor: LOAD_CALIBRATION refused with status 0x1c\n')
        self.assertIn('systemctl start --no-block aurora-touchid-setup.service', calls)


if __name__ == '__main__':
    unittest.main()
