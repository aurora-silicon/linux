#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keep native Apple hardware support explicit in the shared distro fragment.

This checks coverage, not Kconfig dependency resolution or hardware operation.
Package builds still resolve the fragment and reject silently dropped requests.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE_DIRECTORIES = (
    'drivers/soc/apple/', 'drivers/pmdomain/apple/', 'drivers/phy/apple/',
    'drivers/gpu/drm/apple/', 'drivers/gpu/drm/asahi/', 'sound/soc/apple/',
    'drivers/net/wireless/apple/',
)
# Native options that stay off until their stated prerequisites ship; the fragment must say
# so with "# CONFIG_<name> is not set".
DELIBERATELY_OFF = {'MT7932_AWDL'}
BLOCK = re.compile(r'^(?:menuconfig|config) ([A-Z0-9_]+)\n(.*?)(?=^(?:menuconfig|config) |\Z)',
                   re.M | re.S)


def hardware_options(path, text):
    for name, body in BLOCK.findall(text):
        native = ('ARCH_APPLE' in body or path.startswith(NATIVE_DIRECTORIES)
                  or name.startswith(('PCIE_APPLE', 'MT7932')))
        if not native or name in DELIBERATELY_OFF or re.search(r'KUNIT|DEBUG|TEST|DIAG|TRACE|RESEARCH', name):
            continue
        if re.search(r'depends on.*\b(?:X86|PPC|PPC_PMAC|MAC)\b', body):
            continue
        kind = re.search(r'^\s*(bool|tristate)\s+"', body, re.M)
        if kind:
            yield name, kind[1]


def missing_requests(options, fragment):
    values = dict(re.findall(r'^CONFIG_([A-Z0-9_]+)=([^\n]+)$', fragment, re.M))
    return [name for name, kind in options
            if values.get(name) not in (('y',) if kind == 'bool' else ('y', 'm'))]


class HardwareDefaultsTests(unittest.TestCase):
    def test_shared_profile_covers_native_driver_options(self):
        options = []
        for directory in ('drivers', 'sound'):
            for path in sorted((ROOT / directory).rglob('Kconfig*')):
                if path.is_file():
                    options.extend(hardware_options(path.relative_to(ROOT).as_posix(), path.read_text()))
        self.assertGreater(len(options), 40)
        fragment = (ROOT / 'arch/arm64/configs/asahi.config').read_text()
        self.assertEqual(missing_requests(options, fragment), [])

    def test_deliberately_off_options_are_explicitly_off(self):
        fragment = (ROOT / 'arch/arm64/configs/asahi.config').read_text()
        for name in sorted(DELIBERATELY_OFF):
            self.assertIn(f'# CONFIG_{name} is not set\n', fragment)
            self.assertNotRegex(fragment, rf'(?m)^CONFIG_{name}=')

    def test_a_new_driver_cannot_be_forgotten_even_with_a_kconfig_default(self):
        text = 'config NEW_APPLE_DRIVER\n\ttristate "New hardware"\n\tdepends on ARCH_APPLE\n\tdefault m\n'
        options = list(hardware_options('drivers/example/Kconfig', text))
        self.assertEqual(missing_requests(options, ''), ['NEW_APPLE_DRIVER'])
        self.assertEqual(missing_requests(options, 'CONFIG_NEW_APPLE_DRIVER=m\n'), [])

    def test_boolean_features_must_be_built_in(self):
        options = [('NEW_FEATURE', 'bool')]
        self.assertEqual(missing_requests(options, 'CONFIG_NEW_FEATURE=m\n'), ['NEW_FEATURE'])
        self.assertEqual(missing_requests(options, 'CONFIG_NEW_FEATURE=y\n'), [])

    def test_native_subdirectory_options_do_not_need_a_repeated_arch_dependency(self):
        text = 'config NATIVE_HELPER\n\tbool "Native helper"\n'
        self.assertEqual(list(hardware_options('drivers/soc/apple/Kconfig', text)),
                         [('NATIVE_HELPER', 'bool')])

    def test_x86_and_instrumentation_are_not_hardware_defaults(self):
        text = ('config LEGACY_APPLE\n\ttristate "Intel helper"\n\tdepends on X86\n'
                'config APPLE_KUNIT_TEST\n\tbool "Fixtures"\n'
                'config APPLE_SEP_KEY_RESEARCH\n\tbool "Measurements"\n')
        self.assertEqual(list(hardware_options('drivers/soc/apple/Kconfig', text)), [])


if __name__ == '__main__':
    unittest.main()
