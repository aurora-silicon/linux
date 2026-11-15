#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit 6 GHz classes and bind authenticated LP/VLP table data for fullmac."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct

SPEC = importlib.util.spec_from_file_location(
    'six_policy', Path(__file__).with_name('mt7932-policy-6ghz.py'))
six = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(six)
CLASSES = (('VLP', 1, '2'), ('LP', 2, '1'), ('SP', 3, '3'))


def class_data(country, name, channels, legal, groups):
    """Table facts only. Class IDs are J7RP IDs, not firmware selectors."""
    parsed = six.rows(groups, channels)
    records = [(ch, six.power.expand_record(six.power.value_image(parsed[ch])))
               for ch in channels]
    mode = next(mode for label, _, mode in CLASSES if label == name)
    mask = sum(1 << ((ch - 1) // 4) for (ch, values), permission in zip(records, legal)
               if 1 <= ch <= 233 and ch % 4 == 1 and mode in permission.split('/')
               and any(value not in (0xc4, 0x7f) for value in values))
    pad = six.power.one_ss_pad([(ch, six.power.parsed_image(parsed[ch])) for ch in channels])
    return mask, pad, records


def audit(channels, countries, powers):
    result = {}
    for name, _, _ in CLASSES:
        sections = {key.split(',')[0]: groups for key, groups in powers.items()
                    if key.endswith(',' + name)}
        country_sections = {cc: groups for cc, groups in sections.items()
                            if re.fullmatch(r'[A-Z]{2}', cc)}
        eligible, skipped = {}, {}
        for cc, groups in sorted(country_sections.items()):
            if cc not in countries:
                skipped[cc] = 'no 6 GHz country matrix row'
                continue
            mask, _, _ = class_data(cc, name, channels, countries[cc], groups)
            if mask:
                eligible[cc] = mask.bit_count()
            else:
                skipped[cc] = 'no powered primary for this class'
        result[name] = {
            'sections': len(sections), 'country_sections': len(country_sections),
            'nonempty_country_sections': sum(any(groups.values())
                                             for groups in country_sections.values()),
            'noncountry_labels': sorted(set(sections) - set(country_sections)),
            'eligible_countries': eligible, 'eligible_count': len(eligible), 'skipped': skipped,
        }
    return result


def section(country, name, channels, legal, groups):
    mask, pad, records = class_data(country, name, channels, legal, groups)
    if not mask:
        return None
    class_id = next(value for label, value, _ in CLASSES if label == name)
    if name == 'SP':
        raise ValueError('SP is audit-only')
    # 24-byte section header, 32-byte pad, then count * 122-byte table records.
    body = pad + b''.join(bytes((ch,)) + values for ch, values in records)
    header = struct.pack('<BBBBIQ2s6s', class_id, int(name == 'LP'), 1, len(records),
                         24 + len(body), mask, country.encode('ascii'), bytes(6))
    return header + body


def archive(legacy, country, channels, legal, powers):
    six.validate_legacy(legacy, country)
    sections = [section(country, name, channels, legal, powers[country + ',' + name])
                for name in ('VLP', 'LP') if country + ',' + name in powers]
    sections = [data for data in sections if data is not None]
    if not any(data[0] == 2 for data in sections):
        raise ValueError('archive requires usable LPI data')
    # Binding revision 0 explicitly means NOT firmware command payloads.
    directory = struct.pack('<4sBBBBII', b'6GCL', 1, len(sections), 0, 0,
                            16 + sum(map(len, sections)), 0)
    result = bytearray(legacy) + directory + b''.join(sections)
    struct.pack_into('<H', result, 4, 3)
    struct.pack_into('<I', result, 12, len(result))
    return bytes(result)


def parse_archive(data, country):
    """Strict host validator for unbound v3; the runtime driver rejects v3."""
    if len(data) < 16 or len(data) > 65536 or data[:4] != b'J7RP' or (
            struct.unpack_from('<H', data, 4)[0] != 3 or data[6:8] != country.encode('ascii')
            or struct.unpack_from('<I', data, 12)[0] != len(data)):
        raise ValueError('archive header')
    at = 16 + data[8] * 328
    for _ in range(13):
        if at + 44 > len(data):
            raise ValueError('truncated legacy table')
        at += struct.unpack_from('<H', data, at + 2)[0]
    legacy = bytearray(data[:at])
    if len(legacy) < 16:
        raise ValueError('legacy prefix')
    struct.pack_into('<H', legacy, 4, 1)
    struct.pack_into('<I', legacy, 12, len(legacy))
    six.validate_legacy(legacy, country)
    if len(data) - at < 16:
        raise ValueError('truncated directory')
    magic, revision, count, binding, flags, size, reserved = struct.unpack_from('<4sBBBBII', data, at)
    if (magic != b'6GCL' or revision != 1 or count not in (1, 2) or binding or flags
            or reserved or size != len(data) - at):
        raise ValueError('class directory')
    at += 16
    result, previous = {}, 0
    for _ in range(count):
        if len(data) - at < 24:
            raise ValueError('truncated class')
        cid, indoor, units, records, length, mask, alpha2, reserved = struct.unpack_from(
            '<BBBBIQ2s6s', data, at)
        if (cid not in (1, 2) or cid <= previous or indoor != int(cid == 2) or units != 1
                or records != 110 or length != 24 + 32 + records * 122 or length > len(data) - at
                or not 0 < mask < (1 << 59) or alpha2 != country.encode('ascii') or any(reserved)):
            raise ValueError('class header')
        start = at + 56
        channels = [data[start + index * 122] for index in range(records)]
        expected = [2] + [ch for ch in range(1, 234, 2)
                          if six.power.channel_width_class(3, ch) <= 3 and ch != 231]
        if channels != expected:
            raise ValueError('class channel order')
        for bit in range(59):
            if mask & (1 << bit):
                index = channels.index(1 + 4 * bit)
                values = data[start + index * 122 + 1:start + (index + 1) * 122]
                if all(value in (0xc4, 0x7f) for value in values):
                    raise ValueError('primary has no power data')
        result[cid] = mask
        previous = cid
        at += length
    if at != len(data) or 2 not in result:
        raise ValueError('trailing data or missing LPI')
    return result


def bind(archive_data, country):
    """v4: unchanged class data, binding revision 1 (SPEC 2026-10-31)."""
    parse_archive(archive_data, country)
    data = bytearray(archive_data)
    at = 16 + data[8] * 328
    for _ in range(13):
        at += struct.unpack_from('<H', data, at + 2)[0]
    struct.pack_into('<H', data, 4, 4)
    data[at + 6] = 1
    return bytes(data)


def build(channels, countries, powers, legacy_dir, requested=None):
    summary = audit(channels, countries, powers)
    candidates = summary['LP']['eligible_countries']
    selected = sorted(set(requested) if requested is not None else candidates)
    built, skipped = {}, {}
    for cc in selected:
        if cc not in candidates:
            skipped[cc] = 'no usable LP data with a matching matrix row'
            continue
        source = legacy_dir / (cc + '.bin')
        if not source.is_file():
            skipped[cc] = 'no matching v1 package'
            continue
        blob = archive(source.read_bytes(), cc, channels, countries[cc], powers)
        parse_archive(blob, cc)
        built[cc] = blob
    return built, {'classes': summary, 'skipped': skipped, 'runtime_supported': False,
                   'binding_revision': 0, 'countries': {
                       cc: {'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob)}
                       for cc, blob in sorted(built.items())}}


def publish(output, built, runtime=False):
    if output.exists() and any(output.iterdir()):
        raise ValueError('output directory must be absent or empty')
    version = 4 if runtime else 3
    inventory = ''.join(f'policy-v{version}/{cc}.bin\t{hashlib.sha256(blob).hexdigest()}\n'
                        for cc, blob in sorted(built.items())).encode('ascii')
    digest = hashlib.sha256(inventory).hexdigest()
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory = output / f'policy-v{version}'
    directory.mkdir(mode=0o700)
    files = [(directory / (cc + '.bin'), blob) for cc, blob in sorted(built.items())]
    files += [(output / 'inventory.tsv', inventory),
              (output / 'inventory.sha256', (digest + '  inventory.tsv\n').encode('ascii'))]
    for target, data in files:
        with target.open('xb') as stream:
            target.chmod(0o600)
            stream.write(data)
    return {'entries': len(built), 'sha256': digest}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tables', type=Path, required=True)
    parser.add_argument('--legacy-dir', type=Path,
                        help='required for archive generation; omit for audit only')
    parser.add_argument('--output-dir', type=Path,
                        help='opt-in unbound v3 archive root; never install in policy[-v2]/')
    parser.add_argument('--runtime', action='store_true',
                        help='emit policy-v4/ with SPEC binding revision 1; v3 remains unbound')
    parser.add_argument('--country', action='append', type=six.alpha2)
    args = parser.parse_args(argv)
    if bool(args.legacy_dir) != bool(args.output_dir):
        parser.error('--legacy-dir and --output-dir must be used together')
    if args.runtime and not args.output_dir:
        parser.error('--runtime requires archive generation')
    if args.country and not args.output_dir:
        parser.error('--country requires archive generation')
    inputs = {name: (args.tables / name).read_bytes() for name in six.PINS}
    if any(hashlib.sha256(data).hexdigest() != six.PINS[name] for name, data in inputs.items()):
        raise ValueError('unrecognized input revision')
    channels, countries = six.matrix(inputs[six.DB_NAME].decode('ascii'))
    powers = six.sections(inputs[six.SIX_NAME].decode('ascii'))
    if args.output_dir:
        built, report = build(channels, countries, powers, args.legacy_dir, args.country)
        if args.runtime:
            built = {cc: bind(blob, cc) for cc, blob in built.items()}
            report['runtime_supported'] = True
            report['binding_revision'] = 1
            report['countries'] = {cc: {'sha256': hashlib.sha256(blob).hexdigest(),
                                        'bytes': len(blob)} for cc, blob in sorted(built.items())}
        report['inventory'] = publish(args.output_dir, built, args.runtime)
    else:
        report = {'classes': audit(channels, countries, powers), 'runtime_supported': False}
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
