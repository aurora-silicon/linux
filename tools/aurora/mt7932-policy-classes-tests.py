#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic interoperability and rejection tests for unbound class archives."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gen = load('classes', 'mt7932-policy-classes.py')
fixture = load('fixtures', 'mt7932-policy-6ghz-tests.py')


class ClassTests(unittest.TestCase):
    def package(self, names=('VLP', 'LP'), mode='1/2/3'):
        powers = {'ZZ,' + name: fixture.groups() for name in names}
        return gen.archive(fixture.legacy('ZZ'), 'ZZ', fixture.CHANNELS,
                           [mode] * 110, powers)

    def test_independent_class_masks_and_layout(self):
        powers = {'ZZ,LP': fixture.groups(), 'ZZ,VLP': fixture.groups()}
        legal = ['1'] * 110
        legal[fixture.CHANNELS.index(5)] = '2'
        blob = gen.archive(fixture.legacy('ZZ'), 'ZZ', fixture.CHANNELS, legal, powers)
        masks = gen.parse_archive(blob, 'ZZ')
        self.assertEqual(masks[1], 2)
        self.assertEqual(masks[2], ((1 << 59) - 1) ^ 2)
        prefix = len(fixture.legacy('ZZ'))
        self.assertEqual(len(blob), prefix + 16 + 2 * (24 + 32 + 110 * 122))
        self.assertEqual(blob[prefix:prefix + 8], b'6GCL\x01\x02\0\0')
        self.assertEqual(blob[prefix + 16:prefix + 20], bytes((1, 0, 1, 110)))
        self.assertEqual(blob[prefix + 16 + 13476:prefix + 20 + 13476], bytes((2, 1, 1, 110)))
        restored = bytearray(blob[:prefix])
        struct.pack_into('<H', restored, 4, 1)
        struct.pack_into('<I', restored, 12, len(restored))
        self.assertEqual(restored, fixture.legacy('ZZ'))

    def test_lpi_without_vlp_and_no_sp_serialization(self):
        blob = self.package(('LP', 'SP'))
        self.assertEqual(gen.parse_archive(blob, 'ZZ'), {2: (1 << 59) - 1})
        self.assertEqual(len(blob), len(fixture.legacy('ZZ')) + 16 + 13476)
        with self.assertRaisesRegex(ValueError, 'SP is audit-only'):
            gen.section('ZZ', 'SP', fixture.CHANNELS, ['3'] * 110, fixture.groups())
        with self.assertRaisesRegex(ValueError, 'requires usable LPI'):
            self.package(('VLP', 'SP'))

    def test_sentinel_only_lp_never_grants(self):
        empty = {name: [] for name in fixture.gen.GROUPS}
        mask, _, _ = gen.class_data('ZZ', 'LP', fixture.CHANNELS, ['1'] * 110, empty)
        self.assertEqual(mask, 0)
        self.assertIsNone(gen.section('ZZ', 'LP', fixture.CHANNELS, ['1'] * 110, empty))

    def test_audit_distinguishes_sections_labels_and_usable_countries(self):
        empty = {name: [] for name in fixture.gen.GROUPS}
        powers = {'ZZ,LP': fixture.groups(), 'ZZ,SP': empty, 'AA,LP': empty,
                  'BB,LP': fixture.groups(), 'A2,LP': fixture.groups()}
        report = gen.audit(fixture.CHANNELS, {'ZZ': ['1/3'] * 110, 'AA': ['1'] * 110}, powers)
        self.assertEqual(report['LP']['sections'], 4)
        self.assertEqual(report['LP']['country_sections'], 3)
        self.assertEqual(report['LP']['noncountry_labels'], ['A2'])
        self.assertEqual(report['LP']['eligible_countries'], {'ZZ': 59})
        self.assertEqual(report['LP']['eligible_count'], 1)
        self.assertEqual(report['SP']['country_sections'], 1)
        self.assertEqual(report['SP']['nonempty_country_sections'], 0)
        self.assertEqual(report['SP']['eligible_count'], 0)

    def test_truncation_reserved_fields_and_unknown_bindings_rejected(self):
        blob = self.package()
        for size in range(len(blob)):
            with self.assertRaises(ValueError, msg=f'truncation {size}'):
                gen.parse_archive(blob[:size], 'ZZ')
        ext = len(fixture.legacy('ZZ'))
        first = ext + 16
        changes = [(4, 2), (6, ord('A')), (10, 1), (ext, 0), (ext + 4, 2),
                   (ext + 5, 0), (ext + 5, 3), (ext + 6, 1), (ext + 7, 1),
                   (ext + 8, 0), (ext + 12, 1), (first, 3), (first + 1, 1),
                   (first + 2, 0), (first + 3, 109), (first + 4, 0),
                   (first + 15, 8), (first + 16, ord('A')), (first + 18, 1),
                   (first + 56, 1), (first + 13476, 1), (first + 13476 + 1, 0)]
        for offset, value in changes:
            bad = bytearray(blob)
            self.assertNotEqual(bad[offset], value)
            bad[offset] = value
            with self.assertRaises(ValueError, msg=f'offset {offset}'):
                gen.parse_archive(bad, 'ZZ')
        bad = bytearray(blob + b'\0')
        struct.pack_into('<I', bad, 12, len(bad))
        with self.assertRaises(ValueError):
            gen.parse_archive(bad, 'ZZ')

    def test_atomic_plan_determinism_and_private_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / 'legacy'
            base.mkdir()
            for cc in ('AA', 'ZZ'):
                (base / (cc + '.bin')).write_bytes(fixture.legacy(cc))
            powers = {cc + ',LP': fixture.groups() for cc in ('AA', 'ZZ', 'BB')}
            countries = {cc: ['1'] * 110 for cc in ('AA', 'ZZ', 'BB')}
            built, report = gen.build(fixture.CHANNELS, countries, powers, base)
            again, other = gen.build(fixture.CHANNELS, countries, powers, base, ['ZZ', 'AA', 'BB'])
            self.assertEqual(built, again)
            self.assertEqual(report, other)
            self.assertEqual(report['skipped'], {'BB': 'no matching v1 package'})
            self.assertFalse(report['runtime_supported'])
            inventory = gen.publish(root / 'out', built)
            data = (root / 'out/inventory.tsv').read_bytes()
            self.assertEqual(inventory['sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(data.splitlines()[0].split(b'\t')[0], b'policy-v3/AA.bin')
            self.assertEqual((root / 'out/policy-v3/ZZ.bin').stat().st_mode & 0o777, 0o600)
            with self.assertRaises(ValueError):
                gen.publish(root / 'out', built)
            (base / 'ZZ.bin').write_bytes(b'invalid')
            with self.assertRaises(ValueError):
                gen.build(fixture.CHANNELS, countries, powers, base)

    def test_runtime_binding_preserves_class_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, base = fixture.fixtures(root)
            powers = {'ZZ,LP': fixture.groups(), 'ZZ,VLP': fixture.groups()}
            # Reuse the fixture's explicit grammar, not real firmware inputs.
            channels = fixture.CHANNELS
            blob = gen.archive((base / 'ZZ.bin').read_bytes(), 'ZZ', channels,
                               ['1/2'] * 110, powers)
            bound = gen.bind(blob, 'ZZ')
            restored = bytearray(bound)
            at = 16 + restored[8] * 328
            for _ in range(13):
                at += struct.unpack_from('<H', restored, at + 2)[0]
            self.assertEqual(struct.unpack_from('<H', bound, 4)[0], 4)
            self.assertEqual(bound[at + 6], 1)
            struct.pack_into('<H', restored, 4, 3)
            restored[at + 6] = 0
            self.assertEqual(bytes(restored), blob)
            with self.assertRaises(ValueError):
                gen.parse_archive(bound, 'ZZ')
            gen.publish(root / 'runtime', {'ZZ': bound}, runtime=True)
            self.assertEqual((root / 'runtime/policy-v4/ZZ.bin').read_bytes(), bound)

    def test_cli_authentication_and_audit_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tables, base = fixture.fixtures(root)
            # The fixture has a BB LP section; select LP rather than VLP permissions.
            text = (tables / gen.six.DB_NAME).read_text().replace(',2,', ',1,')
            (tables / gen.six.DB_NAME).write_text(text)
            argv = ['--tables', str(tables)]
            with self.assertRaises(ValueError):
                gen.main(argv)
            pins = {name: hashlib.sha256((tables / name).read_bytes()).hexdigest()
                    for name in gen.six.PINS}
            with mock.patch.dict(gen.six.PINS, pins, clear=True):
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    gen.main(argv)
                report = json.loads(stdout.getvalue())
                self.assertFalse(report['runtime_supported'])
                self.assertEqual(report['classes']['LP']['eligible_count'], 1)
                self.assertFalse((root / 'out').exists())
                with contextlib.redirect_stdout(io.StringIO()):
                    gen.main(argv + ['--legacy-dir', str(base), '--output-dir', str(root / 'out')])
                self.assertEqual(gen.parse_archive((root / 'out/policy-v3/BB.bin').read_bytes(), 'BB').keys(), {2})


if __name__ == '__main__':
    unittest.main()
