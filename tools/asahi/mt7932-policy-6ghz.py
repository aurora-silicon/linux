#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extend J7RP country files from authenticated IZUBA text data, without blobs."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys


def _sibling(name):
    """Load a module from this directory, also under python3 -I or by path."""
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


power = _sibling('mt7932_power')

SIX_NAME = 'IZUBA_TxPwrLimit6G_MT79x1.dat'
DB_NAME = 'IZUBA_db.dat'
PINS = {
    SIX_NAME: 'bc22e249526867b62542ef61231c14b7027ae49f8c926ad6d73029c742dc53bc',
    DB_NAME: 'a61e0b70cbc9a381fc41ff9afa8f036cbd032cadad66209134d4a5adfa18c8b2',
}
GROUPS = ('ofdm', 'ru26', 'ru52', 'ru106', 'ru242', 'ru484', 'ru996', 'ru996X2')


class NoVLPPrimary(ValueError):
    """An otherwise valid country has no usable VLP primary."""


def alpha2(value):
    if not re.fullmatch(r'[A-Z]{2}', value):
        raise ValueError('country must be two uppercase ASCII letters')
    return value


def sections(text):
    result, current, group = {}, None, None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', '{Ver:', '<Ver:')):
            continue
        if line.startswith('[') and line.endswith(']'):
            if group is not None or line[1:-1] in result:
                raise ValueError('duplicate section or unclosed group')
            current = result.setdefault(line[1:-1], {})
        elif line.startswith('</') and line.endswith('>'):
            if group is None or line[2:-1] not in (group, 'end'):
                raise ValueError('unexpected group end')
            group = None
        elif line.startswith('<') and line.endswith('>'):
            name = line[1:-1].split(',')[0]
            if current is None or group is not None or name in current or name not in GROUPS:
                raise ValueError('unknown or duplicate power group')
            group = name
            current[group] = []
        else:
            if current is None or group is None:
                raise ValueError('row outside group')
            fields = [x.strip() for x in line.split(',')]
            if len(fields) != 1 + power.GROUP_WIDTH[group] or not fields[0]:
                raise ValueError('power row shape')
            for cell in fields[1:]:
                if cell not in ('X', 'x', 'Y') and not re.fullmatch(r'-?\d+', cell):
                    raise ValueError('power cell grammar')
                if cell not in ('X', 'x', 'Y') and not -128 <= int(cell) <= 127:
                    raise ValueError('power cell range')
            current[group].append((fields[0], fields[1:]))
    if group is not None:
        raise ValueError('unclosed power group')
    return result


def matrix(text):
    match = re.search(r'<6gCC,([^>]*)>\s*(.*?)</', text, re.S)
    if not match:
        raise ValueError('6 GHz country matrix absent')
    columns = [x.strip() for x in match[1].split(',')]
    channels = [int(x[2:]) for x in columns if re.fullmatch(r'6g\d+', x)]
    if len(channels) != 110 or len(set(channels)) != 110 or columns[110:] != [
            'vht40', 'vht80', 'vht160', 'c2clpi', 'vlpp2p', 'flag']:
        raise ValueError('6 GHz matrix columns')
    countries = {}
    for raw in match[2].splitlines():
        if not raw.strip() or raw.lstrip().startswith('#'):
            continue
        fields = [x.strip() for x in raw.split(',')]
        if len(fields) != 118 or fields[-1] or fields[0] in countries:
            raise ValueError('6 GHz country matrix row')
        if any(not re.fullmatch(r'[0-3](?:/[1-3])*', x) for x in fields[1:111]):
            raise ValueError('6 GHz operating-class set')
        countries[fields[0]] = fields[1:-1]
    return channels, countries


def rows(groups, channels):
    if set(groups) != set(GROUPS):
        raise ValueError('incomplete 6 GHz power groups')
    result = {ch: {name: ['X'] * power.GROUP_WIDTH[name] for name in power.GROUPS}
              for ch in channels}
    for name, records in groups.items():
        index = power.GROUPS.index(name)
        for label, cells in records:
            start, end, cdd, kind = power._power_span(label)
            if start not in result or end not in result:
                raise ValueError('power row outside channel namespace')
            width = power.channel_width_class(3, start)
            if kind == 'plain' or width == 0:
                power._put(result[start], index, 0, cells)
                if index == 1 and (width in (1, 2, 3) or start == 2):
                    power._put(result[start], 2, 0, cells)
                continue
            dest, offset = power._bonded_destination(
                index, width, power.channel_width_class(3, end), cdd)
            power._put(result[end], dest, offset, cells)
    return result


def pages(country, channels, parsed):
    alpha2 = country.encode('ascii')
    pad = power.one_ss_pad([(ch, power.parsed_image(parsed[ch])) for ch in channels])
    result = []
    for start in range(0, len(channels), 8):
        chunk = channels[start:start + 8]
        header = bytearray(44)
        header[0:2] = b'\x03\x2a'
        struct.pack_into('<H', header, 2, 44 + 122 * len(chunk))
        header[4:7] = bytes((len(chunk), 3, int(start + 8 >= len(channels))))
        header[8:10] = alpha2
        header[12:44] = pad
        data = b''.join(bytes((ch,)) + power.expand_record(power.value_image(parsed[ch]))
                        for ch in chunk)
        result.append((0x5d, bytes(header) + data))
    for start in range(0, len(channels), 8):
        chunk = channels[start:start + 8]
        header = bytearray(44)
        header[4:7] = bytes((len(chunk), 3, int(start + 8 >= len(channels))))
        result.append((0x95, bytes(header) + b''.join(
            bytes((ch,)) + b'\x7f' * 32 for ch in chunk)))
    return result


def validate_legacy(data, country):
    if (not 16 <= len(data) <= 16384 or data[:4] != b'J7RP'
            or struct.unpack_from('<H', data, 4)[0] != 1
            or data[6:8] != country.encode('ascii') or data[8] not in (2, 3)
            or data[9:12] != b'\x0d\0\0' or struct.unpack_from('<I', data, 12)[0] != len(data)):
        raise ValueError('legacy country package header')
    at = 16
    for _ in range(data[8]):
        if len(data) - at < 328 or struct.unpack_from('<I', data, at)[0] or not (
                1 <= struct.unpack_from('<I', data, at + 4)[0] <= 320):
            raise ValueError('legacy mode record')
        at += 328
    for index in range(13):
        if len(data) - at < 44:
            raise ValueError('truncated legacy table')
        size = struct.unpack_from('<H', data, at + 2)[0]
        if size < 44 or size > 1020 or size > len(data) - at:
            raise ValueError('legacy table size')
        page = data[at:at + size]
        if 1 <= index <= 9:
            count = 5 if index in (2, 9) else 8
            if (page[:2] != b'\x03\x0a' or page[4:8] != bytes((count,
                    1 if index <= 2 else 2, int(index in (2, 9)), 0)) or
                    page[8:12] != country.encode('ascii') + b'\0\0' or
                    size != 44 + count * 122):
                raise ValueError('legacy per-rate table')
        else:
            slot = index - 9 if index else 0
            if size != (532, 144, 58, 265)[slot] or page[:2] != b'\0\x02' or (
                    page[7] != (5, 1, 4, 2)[slot]):
                raise ValueError('legacy policy table')
            if index == 10 and page[8:12] != country.encode('ascii') + b'\0\0':
                raise ValueError('legacy SAR country')
        at += size
    if at != len(data):
        raise ValueError('legacy package trailing data')


def extend(legacy, country, channels, legal, parsed):
    validate_legacy(legacy, country)
    allowed = 0
    for ch, modes in zip(channels, legal):
        if ch != 2 and 1 <= ch <= 233 and ch % 4 == 1 and '2' in modes.split('/'):
            values = power.expand_record(power.value_image(parsed[ch]))
            if any(x not in (0xc4, 0x7f) for x in values):
                allowed |= 1 << ((ch - 1) // 4)
    if not allowed:
        raise NoVLPPrimary('country has no VLP primary with power data')
    messages = pages(country, channels, parsed)
    extension = bytearray(24)
    extension[:8] = b'6GRP' + bytes((1, 14, len(messages), 0))
    struct.pack_into('<Q', extension, 8, allowed)
    extension[16:18] = country.encode('ascii')
    extension[18] = 1  # Native half-dBm per-rate cell contract.
    for cid, body in messages:
        extension += struct.pack('<BBH', cid, 0, len(body)) + body
    out = bytearray(legacy) + extension
    struct.pack_into('<H', out, 4, 2)
    struct.pack_into('<I', out, 12, len(out))
    if len(out) > 65536:
        raise ValueError('extended package too large')
    return bytes(out), allowed, messages


def build(channels, countries, powers, legacy_dir, requested=None):
    """Plan the complete output before writing anything; corrupt inputs are fatal."""
    vlp = {key[:-4] for key in powers if key.endswith(',VLP')}
    selected = sorted(set(requested) if requested is not None else set(countries) | vlp)
    built, report, skipped = {}, {}, {}
    for country in selected:
        if not re.fullmatch(r'[A-Z]{2}', country):
            skipped[country] = 'not an uppercase alpha2 country'
            continue
        if country not in vlp:
            skipped[country] = 'no VLP section'
            continue
        if country not in countries:
            skipped[country] = 'no 6 GHz country matrix row'
            continue
        source = legacy_dir / (country + '.bin')
        if not source.is_file():
            skipped[country] = 'no matching v1 package'
            continue
        parsed = rows(powers[country + ',VLP'], channels)
        try:
            blob, allowed, messages = extend(source.read_bytes(), country, channels,
                                             countries[country], parsed)
        except NoVLPPrimary as error:
            skipped[country] = str(error)
            continue
        built[country] = blob
        report[country] = {'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob),
                           'allowed_primaries': allowed.bit_count(), 'power_pages': len(messages)}
    return built, {'countries': report, 'skipped': skipped,
                   'counts': {'candidates': len(selected), 'generated': len(built),
                              'skipped': len(skipped), 'vlp_sections': len(vlp),
                              'matrix_countries': len(countries)}}


def publish(output_dir, built):
    """Publish a fresh inventory and packages; never mix with an earlier run."""
    if output_dir.exists() and any(output_dir.iterdir()):
        raise ValueError('output directory must be absent or empty')
    inventory = ''.join(f'policy-v2/{country}.bin\t{hashlib.sha256(blob).hexdigest()}\n'
                        for country, blob in sorted(built.items())).encode('ascii')
    digest = hashlib.sha256(inventory).hexdigest()
    output_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    policy_dir = output_dir / 'policy-v2'
    policy_dir.mkdir(mode=0o700)
    files = [(policy_dir / (country + '.bin'), blob) for country, blob in sorted(built.items())]
    files += [(output_dir / 'inventory.tsv', inventory),
              (output_dir / 'inventory.sha256', (digest + '  inventory.tsv\n').encode('ascii'))]
    for target, data in files:
        with target.open('xb') as stream:
            target.chmod(0o600)
            stream.write(data)
    return {'path': 'inventory.tsv', 'sha256': digest, 'entries': len(built)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tables', type=Path, required=True)
    parser.add_argument('--legacy-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='fresh root for policy-v2/, inventory.tsv and inventory.sha256')
    parser.add_argument('--country', action='append', type=alpha2,
                        help='uppercase alpha2; repeat to select countries (default: all eligible)')
    args = parser.parse_args(argv)
    inputs = {name: (args.tables / name).read_bytes() for name in PINS}
    if any(hashlib.sha256(data).hexdigest() != PINS[name] for name, data in inputs.items()):
        raise ValueError('unrecognized input revision')
    channels, countries = matrix(inputs[DB_NAME].decode('ascii'))
    powers = sections(inputs[SIX_NAME].decode('ascii'))
    built, report = build(channels, countries, powers, args.legacy_dir, args.country)
    report['inventory'] = publish(args.output_dir, built)
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
