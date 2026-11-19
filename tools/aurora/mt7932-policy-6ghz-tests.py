#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic country-package tests; no firmware or board data is embedded."""
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

SPEC = importlib.util.spec_from_file_location('six_policy',
    Path(__file__).with_name('mt7932-policy-6ghz.py'))
gen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gen)
CHANNELS = [2] + [ch for ch in range(1, 234, 2)
                  if gen.power.channel_width_class(3, ch) <= 3 and ch != 231]


def legacy(country='AU', modes=2):
    out = bytearray(b'J7RP' + struct.pack('<H', 1) + country.encode()
                    + bytes((modes, 13)) + bytes(6))
    for _ in range(modes):
        mode = bytearray(328)
        struct.pack_into('<I', mode, 4, 1)
        out += mode
    for index in range(13):
        if 1 <= index <= 9:
            count = 5 if index in (2, 9) else 8
            table = bytearray(44 + count * 122)
            table[:2] = b'\x03\x0a'
            table[4:8] = bytes((count, 1 if index <= 2 else 2, int(index in (2, 9)), 0))
            table[8:10] = country.encode()
        else:
            slot = index - 9 if index else 0
            table = bytearray((532, 144, 58, 265)[slot])
            table[1] = 2
            table[7] = (5, 1, 4, 2)[slot]
            if index == 10:
                table[8:10] = country.encode()
        struct.pack_into('<H', table, 2, len(table))
        out += table
    struct.pack_into('<I', out, 12, len(out))
    return bytes(out)


def groups():
    return {name: [(f'ch{ch:03d}', ['20'] * gen.power.GROUP_WIDTH[name])
                   for ch in CHANNELS] for name in gen.GROUPS}


def fixtures(root):
    """ZZ is deliberately outside the former four-country command-line list."""
    tables, base = root / 'tables', root / 'policy'
    tables.mkdir()
    base.mkdir()
    powers = {'ZZ,VLP': groups(), 'AU,VLP': groups(), 'BB,LP': groups(),
              'CC,VLP': {name: [] for name in gen.GROUPS}, 'DD,VLP': groups(),
              'EE,VLP': groups()}
    text = ''
    for section, entries in powers.items():
        text += f'[{section}]\n'
        for name, records in entries.items():
            text += f'<{name},synthetic>\n'
            text += ''.join(','.join([label] + cells) + '\n' for label, cells in records)
            text += f'</{name}>\n'
    (tables / gen.SIX_NAME).write_text(text, encoding='ascii')
    header = [f'6g{ch}' for ch in CHANNELS] + [
        'vht40', 'vht80', 'vht160', 'c2clpi', 'vlpp2p', 'flag']
    text = '<6gCC,' + ','.join(header) + '>\n'
    text += ''.join(country + ',' + ','.join(['2'] * 110 + ['0'] * 6) + ',\n'
                    for country in ('ZZ', 'AU', 'BB', 'CC', 'DD'))
    (tables / gen.DB_NAME).write_text(text + '</end>\n', encoding='ascii')
    for country in ('ZZ', 'AU', 'BB', 'CC', 'EE'):
        (base / (country + '.bin')).write_bytes(legacy(country))
    return tables, base


class SixPolicyTest(unittest.TestCase):
    def run_fixture(self, root, output, requested=()):
        tables, base = root / 'tables', root / 'policy'
        pins = {name: hashlib.sha256((tables / name).read_bytes()).hexdigest()
                for name in gen.PINS}
        stdout = io.StringIO()
        argv = ['--tables', str(tables), '--legacy-dir', str(base), '--output-dir', str(output)]
        for country in requested:
            argv += ['--country', country]
        with mock.patch.dict(gen.PINS, pins, clear=True), contextlib.redirect_stdout(stdout):
            gen.main(argv)
        return json.loads(stdout.getvalue())

    def test_all_eligible_and_skip_reasons(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            report = self.run_fixture(root, root / 'out')
            self.assertEqual(list(report['countries']), ['AU', 'ZZ'])
            self.assertEqual(report['counts'], {'candidates': 6, 'generated': 2, 'skipped': 4,
                                               'vlp_sections': 5, 'matrix_countries': 5})
            self.assertEqual(report['skipped'], {
                'BB': 'no VLP section', 'CC': 'country has no VLP primary with power data',
                'DD': 'no matching v1 package', 'EE': 'no 6 GHz country matrix row'})
            self.assertEqual(sorted(p.name for p in (root / 'out' / 'policy-v2').iterdir()),
                             ['AU.bin', 'ZZ.bin'])

    def test_explicit_countries_sorted_and_deduplicated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            first = self.run_fixture(root, root / 'one', ('ZZ', 'AU', 'ZZ'))
            second = self.run_fixture(root, root / 'two', ('AU', 'ZZ'))
            self.assertEqual(first, second)
            for path in (root / 'one').rglob('*'):
                if path.is_file():
                    other = root / 'two' / path.relative_to(root / 'one')
                    self.assertEqual(path.read_bytes(), other.read_bytes())

    def test_inventory_exact_bytes_digest_and_private_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            out = root / 'out'
            report = self.run_fixture(root, out)
            expected = b''
            for country in ('AU', 'ZZ'):
                path = out / 'policy-v2' / (country + '.bin')
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                expected += f'policy-v2/{country}.bin\t{digest}\n'.encode('ascii')
                self.assertEqual(report['countries'][country]['sha256'], digest)
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual((out / 'inventory.tsv').read_bytes(), expected)
            digest = hashlib.sha256(expected).hexdigest()
            self.assertEqual(report['inventory'],
                             {'path': 'inventory.tsv', 'sha256': digest, 'entries': 2})
            self.assertEqual((out / 'inventory.sha256').read_bytes(),
                             f'{digest}  inventory.tsv\n'.encode())
            self.assertEqual((out / 'policy-v2').stat().st_mode & 0o777, 0o700)

    def test_country_without_vlp_and_world_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            report = self.run_fixture(root, root / 'out', ('BB', 'XZ'))
            self.assertEqual(report['countries'], {})
            self.assertEqual(report['skipped'], {'BB': 'no VLP section', 'XZ': 'no VLP section'})
            self.assertEqual((root / 'out' / 'inventory.tsv').read_bytes(), b'')

    def test_legacy_mismatch_fails_before_publication(self):
        for defect in ('country', 'version', 'table-country'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                _, base = fixtures(root)
                data = bytearray(legacy('ZZ'))
                if defect == 'country':
                    data[6:8] = b'AU'
                elif defect == 'version':
                    data[4] = 2
                else:
                    data[16 + 2 * 328 + 532 + 8:16 + 2 * 328 + 532 + 10] = b'AU'
                (base / 'ZZ.bin').write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'legacy'):
                    self.run_fixture(root, root / 'out')
                self.assertFalse((root / 'out').exists())

    def test_no_power_rows_or_only_sentinels_are_skipped(self):
        for cells in (None, 'X', 'Y'):
            with self.subTest(cells=cells), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'AU.bin').write_bytes(legacy())
                data = {name: [] if cells is None else [
                    ('ch005', [cells] * gen.power.GROUP_WIDTH[name])] for name in gen.GROUPS}
                built, report = gen.build(CHANNELS, {'AU': ['2'] * 110}, {'AU,VLP': data}, root)
                self.assertEqual(built, {})
                self.assertIn('no VLP primary', report['skipped']['AU'])

    def test_unpopulated_legal_primary_cannot_use_other_channel_power(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'AU.bin').write_bytes(legacy())
            data = {name: [('ch009', ['20'] * gen.power.GROUP_WIDTH[name])] for name in gen.GROUPS}
            legal = ['0'] * 110
            legal[CHANNELS.index(5)] = '2'
            built, report = gen.build(CHANNELS, {'AU': legal}, {'AU,VLP': data}, root)
            self.assertEqual(built, {})
            self.assertIn('AU', report['skipped'])

    def test_refuse_output_reuse_preserves_inventory_and_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            out = root / 'out'
            self.run_fixture(root, out)
            before = {p.relative_to(out): p.read_bytes() for p in out.rglob('*') if p.is_file()}
            with self.assertRaisesRegex(ValueError, 'absent or empty'):
                self.run_fixture(root, out, ('AU',))
            after = {p.relative_to(out): p.read_bytes() for p in out.rglob('*') if p.is_file()}
            self.assertEqual(before, after)

    def test_bad_pin_fails_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tables, base = fixtures(root)
            with self.assertRaisesRegex(ValueError, 'unrecognized input revision'):
                gen.main(['--tables', str(tables), '--legacy-dir', str(base),
                          '--output-dir', str(root / 'out')])
            self.assertFalse((root / 'out').exists())

    def test_country_grammar(self):
        for country in ('au', '../AU', 'A', 'ABC', '00', '\u00c5U'):
            with self.subTest(country=country), self.assertRaises(ValueError):
                gen.alpha2(country)

    def test_pseudo_country_is_reported_without_aborting_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            built, report = gen.build(CHANNELS, {'A1': ['2'] * 110}, {}, Path(directory))
            self.assertEqual(built, {})
            self.assertEqual(report['skipped'], {'A1': 'not an uppercase alpha2 country'})

    def test_v1_bodies_preserved_and_documented_sizes(self):
        parsed = gen.rows(groups(), CHANNELS)
        for modes, size in ((2, 28537), (3, 28865)):
            with self.subTest(modes=modes):
                base = legacy(modes=modes)
                data, _, _ = gen.extend(base, 'AU', CHANNELS, ['2'] * 110, parsed)
                self.assertEqual(len(base), 10119 + (modes - 2) * 328)
                self.assertEqual(len(data), size)
                self.assertEqual(struct.unpack_from('<I', data, 12)[0], size)
                self.assertEqual(data[16:len(base)], base[16:])
                self.assertEqual(len(data) - len(base), 18418)
                self.assertEqual(data[len(base):len(base) + 8], b'6GRP\x01\x0e\x1c\0')

    def test_channel_namespace(self):
        self.assertEqual(len(CHANNELS), 110)
        self.assertEqual(CHANNELS[:9], [2, 1, 3, 5, 7, 9, 11, 13, 15])
        self.assertEqual(CHANNELS[-4:], [225, 227, 229, 233])

    def test_four_country_identity_and_permissions(self):
        parsed = gen.rows(groups(), CHANNELS)
        allowed = ['0'] * 110
        allowed[CHANNELS.index(1)] = '3/1'
        allowed[CHANNELS.index(5)] = '1/2'
        allowed[CHANNELS.index(37)] = '2'
        for country in ('AU', 'US', 'CA', 'DE'):
            with self.subTest(country=country):
                base = legacy(country)
                data, mask, messages = gen.extend(base, country, CHANNELS, allowed, parsed)
                self.assertEqual(mask, (1 << 1) | (1 << 9))
                self.assertEqual(data[6:8], country.encode())
                self.assertEqual(struct.unpack_from('<H', data, 4)[0], 2)
                self.assertEqual(data[len(base):len(base) + 4], b'6GRP')
                self.assertEqual(data[len(base) + 18], 1)
                self.assertEqual(len(messages), 28)
                self.assertTrue(all(body[8:10] == country.encode()
                                    for cid, body in messages if cid == 0x5d))

    def test_pages_and_association_selectors(self):
        messages = gen.pages('AU', CHANNELS, gen.rows(groups(), CHANNELS))
        self.assertEqual([cid for cid, _ in messages], [0x5d] * 14 + [0x95] * 14)
        self.assertEqual([messages[n][1][44] for n in (0, 1)], [2, 15])
        self.assertEqual([len(messages[n][1]) for n in (0, 1)], [1020, 1020])
        for cid, page in messages:
            self.assertEqual(page[5], 3)
            self.assertEqual(len(page), 44 + page[4] * (122 if cid == 0x5d else 33))
            if cid == 0x95:
                self.assertEqual(page[:4], bytes(4))
                self.assertEqual(page[8:44], bytes(36))
        self.assertEqual(messages[13][1][6], 1)
        self.assertEqual(messages[27][1][6], 1)

    def test_missing_power_does_not_grant_permission(self):
        parsed = gen.rows(groups(), CHANNELS)
        parsed[5] = {name: ['X'] * width for name, width in gen.power.GROUP_WIDTH.items()}
        allowed = ['2'] * 110
        _, mask, _ = gen.extend(legacy(), 'AU', CHANNELS, allowed, parsed)
        self.assertFalse(mask & (1 << 1))
        self.assertFalse(mask & (1 << 59))

    def test_no_vlp_country_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'no VLP'):
            gen.extend(legacy(), 'AU', CHANNELS, ['1/3'] * 110,
                       gen.rows(groups(), CHANNELS))

    def test_wrong_legacy_country_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'header'):
            gen.validate_legacy(legacy('US'), 'AU')

    def test_all_legacy_truncations_are_refused(self):
        data = legacy()
        for size in range(len(data)):
            with self.assertRaises(ValueError):
                gen.validate_legacy(data[:size], 'AU')

    def test_legacy_internal_table_corruption(self):
        data = bytearray(legacy())
        data[16 + 2 * 328 + 7] = 0
        with self.assertRaisesRegex(ValueError, 'table'):
            gen.validate_legacy(data, 'AU')

    def test_row_overwrite_and_channel_two_padding(self):
        data = groups()
        data['ofdm'].append(('ch002', ['11'] * 8))
        parsed = gen.rows(data, CHANNELS)
        self.assertEqual(parsed[2]['ofdm'], ['11'] * 8)
        self.assertEqual(parsed[2]['ht20'][:8], ['11'] * 8)
        self.assertEqual(parsed[2]['ht40'], ['X'] * 24)

    def test_bonded_and_cdd_rows(self):
        data = groups()
        data['ofdm'].extend([('ch007-003', ['9'] * 8), ('ch007-003-cdd', ['8'] * 8)])
        parsed = gen.rows(data, CHANNELS)
        self.assertEqual(parsed[3]['ht20'][16:24], ['9'] * 8)
        self.assertEqual(parsed[3]['ht40'][:8], ['8'] * 8)

    def test_unrecognized_rows_and_missing_groups(self):
        data = groups()
        data['ofdm'].append(('ch234', ['1'] * 8))
        with self.assertRaisesRegex(ValueError, 'namespace'):
            gen.rows(data, CHANNELS)
        del data['ofdm']
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            gen.rows(data, CHANNELS)

    def test_ascii_grammar_duplicate_and_range(self):
        for text in ('[AU,VLP]\n[AU,VLP]\n',
                     '[AU,VLP]\n<unknown,x>\n',
                     '[AU,VLP]\n<ofdm,x>\nch005,999,1,1,1,1,1,1,1\n</ofdm>\n',
                     '[AU,VLP]\n<ofdm,x>\nch005,1,1\n</ofdm>\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gen.sections(text)

    def test_wire_projection_distinguishes_groups(self):
        row = {name: ['X'] * width for name, width in gen.power.GROUP_WIDTH.items()}
        row['ofdm'] = [str(i) for i in range(8)]
        wire = gen.power.expand_record(gen.power.value_image(row))
        self.assertEqual(len(wire), 121)
        self.assertEqual(wire[:4], bytes((0xc4, 0, 4, 6)))
        self.assertEqual(wire[-9:], bytes([0xc4]) * 9)


if __name__ == '__main__':
    unittest.main(verbosity=2)
