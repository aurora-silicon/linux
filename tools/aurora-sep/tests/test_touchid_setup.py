"""aurora-touchid-setup, on synthetic boot containers, records and device trees only."""
import contextlib
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch


PACKAGE = Path(__file__).resolve().parents[1]


def load(name, module):
    loader = importlib.machinery.SourceFileLoader(module, str(PACKAGE / name))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    loaded = importlib.util.module_from_spec(spec)
    loader.exec_module(loaded)
    return loaded


helper = load('aurora-touchid-setup', 'aurora_touchid_setup')


# ---------------------------------------------------------------- DER records

def der(tag, value):
    if len(value) < 0x80:
        return bytes([tag, len(value)]) + value
    count = (len(value).bit_length() + 7) // 8
    return bytes([tag, 0x80 | count]) + len(value).to_bytes(count, 'big') + value


def text(value):
    return der(0x16, value)


def private(name, value):
    """[PRIVATE NAME] SEQUENCE { IA5String NAME, value }, as IM4M writes it."""
    number = int.from_bytes(name, 'big')
    identifier = [number & 0x7f]
    number >>= 7
    while number:
        identifier.insert(0, 0x80 | number & 0x7f)
        number >>= 7
    body = der(0x30, text(name) + value)
    return bytes([0xff] + identifier) + der(0, body)[1:]


def im4m(objects, container=0x31):
    """An IM4M whose MANB holds `objects`: {name: {property: value element}}, in a SET
    as image manifests have it, or with `container` 0x30 a SEQUENCE as the J714s
    factory seal has it."""
    body = b''.join(private(name, der(0x31, b''.join(private(key, value)
                                                      for key, value in props.items())))
                    for name, props in objects.items())
    manb = private(b'MANB', der(container, body))
    return der(0x30, text(b'IM4M') + der(0x02, b'\x00') + der(0x31, manb)
               + der(0x04, b'signature') + der(0x30, der(0x30, b'')))


def octets(value):
    return der(0x04, value)


def integer(value):
    return der(0x02, value.to_bytes((value.bit_length() + 8) // 8, 'big'))


def im4p(payload, kind=b'FSC2'):
    return der(0x30, text(b'IM4P') + text(kind) + text(b'1.0') + der(0x04, payload))


def img4(payload_element, manifest):
    return der(0x30, text(b'IMG4') + payload_element + der(0xa0, manifest))


def comb(image, trust):
    return der(0x30, text(b'comb')
               + der(0x30, text(b'fdrd') + der(0x04, image))
               + der(0x30, text(b'secb') + der(0x04, trust)))


CHIP, ECID, SERIAL = 0x8132, 0x0011223344556677, b'TESTSERIAL1'
OTHER_ECID = 0x0011223344556678
SENSOR = '00003356-0123456789ABCDEF0123456789ABCDEF'
CALIBRATION = im4p(b'CALB' + bytes(300))
OTHER = im4p(b'CALB' + bytes(299) + b'\x01')
ROOT_KEY = im4p(b'MSRK' + bytes(100), b'MSRk')
ROOT_KEY_FACTORY = img4(ROOT_KEY, im4m({b'MANP': {b'srvn': octets(b'\x01')}}))
FACTORY = img4(CALIBRATION, im4m({b'MANP': {b'srvn': octets(b'\x01')},
                                  b'FSC2': {b'DGST': octets(hashlib.sha384(CALIBRATION).digest())}}))
TRUST = b'trust bundle ' + bytes(64)
OLD_TRUST = b'old trust bundle ' + bytes(64)


def seal(payload=CALIBRATION, serial=SERIAL, extra=None, container=0x31):
    objects = {b'MANP': {b'SrNm': octets(serial), b'BORD': integer(0x26)},
               b'FSC2': {b'DGST': octets(hashlib.sha384(payload).digest()),
                         b'clas': octets(b'\x00' * 4)},
               b'MSRk': {b'DGST': octets(hashlib.sha384(ROOT_KEY).digest())}}
    for name, props in (extra or {}).items():
        objects.setdefault(name, {}).update(props)
    return im4m(objects, container)


def sealed(payload=CALIBRATION, manifest=None, trust=TRUST):
    return comb(img4(payload, manifest or seal(payload)), trust)


def records(**changes):
    out = {
        f'FSC2-{SENSOR}': FACTORY,
        f'MSRk-{SENSOR}': ROOT_KEY_FACTORY,
        f'seal-{CHIP:08X}-{ECID:016X}': seal(),
        f'trustobject-{hashlib.sha256(TRUST).hexdigest().upper()}': TRUST,
        f'trustobject-{hashlib.sha256(OLD_TRUST).hexdigest().upper()}': OLD_TRUST,
        'trustobject-current': TRUST,
    }
    for name, value in changes.items():
        name = name.replace('_', '-')
        if value is None:
            out.pop(name, None)
        else:
            out[name] = value
    return out


IDENTITY = (CHIP, ECID, SERIAL)
# FSCl comb records as carved: a full one, and the looser layout the extractor
# accepts (a secb holding only its name, manifest markers after the IM4P).
FSCL_RECORDS = [
    comb(img4(im4p(b'CALB' + bytes(40), b'FSCl'), im4m({b'MANP': {}})), TRUST),
    der(0x30, text(b'comb')
        + der(0x30, text(b'fdrd') + der(0x04, der(0x30, text(b'IMG4') + im4p(b'CALB' + bytes(40), b'FSCl')
                                                + text(b'IM4M') + text(b'FSCl'))))
        + der(0x30, text(b'secb'))),
]
EXPECTED = sealed()
EXPECTED_ROOT_KEY = sealed(ROOT_KEY, manifest=seal())


# ---------------------------------------------------------------- APFS images

BS = 4096


def checksummed(block):
    block = bytearray(block.ljust(BS, b'\0'))
    struct.pack_into('<Q', block, 0, helper.fletcher64(bytes(block)))
    return bytes(block)


def obj(oid, xid, kind, body):
    return struct.pack('<QQQII', 0, oid, xid, kind, 0) + body


def node(entries, fixed, root=True, leaf=True, oid=0, xid=1):
    """A B-tree node holding (key, value) entries."""
    width = 4 if fixed else 8
    toc, keys, values = b'', b'', b''
    for key, value in entries:
        values = value + values
        if fixed:
            toc += struct.pack('<HH', len(keys), len(values))
        else:
            toc += struct.pack('<HHHH', len(keys), len(key), len(values), len(value))
        keys += key
    flags = (1 if root else 0) | (2 if leaf else 0) | (4 if fixed else 0)
    header = struct.pack('<HHI', flags, 0 if leaf else 1, len(entries)) + \
        struct.pack('<HHHHHHHH', 0, len(toc), 0, 0, 0, 0, 0, 0)
    body = obj(oid, xid, 0x40000002 if root else 0x40000003, header + toc + keys)
    end = BS - (40 if root else 0)
    assert len(body) + len(values) <= end
    return checksummed(body + bytes(end - len(body) - len(values)) + values + bytes(BS - end))


def fs_key(oid, kind, rest=b''):
    return struct.pack('<Q', oid | kind << 60) + rest


def apfs(files, *, role=helper.APFS_ROLE_HARDWARE, flags=1, bsd_flags=0, path=None,
         hashed=True, extra_volume=True, newer=None):
    """A container whose Hardware volume holds `files` in its factory cache.
    `newer` maps to a second, newer checkpoint superblock pointing at another
    container built from those files."""
    blocks = {}
    path = helper.APFS_FACTORY if path is None else path
    entries, next_block, next_oid = [], 20, 16
    entries.append((fs_key(2, 3), inode(2, 0)))
    parent = 2
    for part in path:
        oid, next_oid = next_oid, next_oid + 1
        entries.append((drec_key(parent, part.encode(), hashed), drec(oid, 4)))
        entries.append((fs_key(oid, 3), inode(oid, 0)))
        parent = oid
    for name, data in files.items():
        oid, next_oid = next_oid, next_oid + 1
        entries.append((drec_key(parent, name.encode(), hashed), drec(oid, 8)))
        entries.append((fs_key(oid, 3), inode(oid, len(data), bsd_flags)))
        if data:
            count = (len(data) + BS - 1) // BS
            for index in range(count):
                blocks[next_block + index] = data[index * BS:(index + 1) * BS].ljust(BS, b'\0')
            entries.append((fs_key(oid, 8, struct.pack('<Q', 0)),
                            struct.pack('<QQQ', count * BS, next_block, 0)))
            next_block += count
    entries.sort(key=lambda e: e[0])
    blocks[10] = node(entries, False, oid=1027)
    blocks[9] = node([(struct.pack('<QQ', 1027, 1), struct.pack('<IIQ', 0, BS, 10))], True, oid=9)
    blocks[8] = checksummed(obj(8, 1, 0x4000000b, struct.pack('<IIIIQ', 0, 0, 0, 0, 9)))
    volume = bytearray(1024)
    volume[0:4] = b'APSB'
    struct.pack_into('<Q', volume, 56 - 32, 1 if hashed else 0)
    struct.pack_into('<I', volume, 116 - 32, 2)
    struct.pack_into('<QQ', volume, 128 - 32, 8, 1027)
    struct.pack_into('<Q', volume, 264 - 32, flags)
    volume[704 - 32:704 - 32 + 8] = b'Hardware'
    struct.pack_into('<H', volume, 964 - 32, role)
    blocks[7] = checksummed(obj(1026, 1, 0xd, bytes(volume)))
    volumes = [(1026, 7)]
    if extra_volume:
        preboot = bytearray(volume)
        struct.pack_into('<H', preboot, 964 - 32, 0x10)
        blocks[11] = checksummed(obj(1030, 1, 0xd, bytes(preboot)))
        volumes.append((1030, 11))
    blocks[6] = node([(struct.pack('<QQ', oid, 1), struct.pack('<IIQ', 0, BS, block))
                      for oid, block in volumes], True, oid=6)
    blocks[5] = checksummed(obj(5, 1, 0x4000000b, struct.pack('<IIIIQ', 0, 0, 0, 0, 6)))
    total = max(next_block, 32)

    def superblock(xid):
        body = bytearray(BS - 32)
        body[0:4] = b'NXSB'
        struct.pack_into('<IQ', body, 4, BS, total)
        struct.pack_into('<II', body, 104 - 32, 2, 0)
        struct.pack_into('<Q', body, 112 - 32, 1)
        struct.pack_into('<Q', body, 160 - 32, 5)
        struct.pack_into('<I', body, 180 - 32, 100)
        for index, (oid, _) in enumerate(volumes):
            struct.pack_into('<Q', body, 184 - 32 + 8 * index, oid)
        return checksummed(obj(1, xid, 0x80000001, bytes(body)))

    blocks[0] = blocks[1] = superblock(1)
    image = bytearray(total * BS)
    for number, block in blocks.items():
        image[number * BS:(number + 1) * BS] = block
    return bytes(image)


def drec_key(parent, name, hashed):
    name += b'\0'
    if hashed:
        return fs_key(parent, 9, struct.pack('<I', len(name) | 0x1234 << 10) + name)
    return fs_key(parent, 9, struct.pack('<H', len(name)) + name)


def drec(oid, kind):
    return struct.pack('<QQH', oid, 0, kind)


def inode(oid, size, bsd_flags=0):
    fixed = bytearray(92)
    struct.pack_into('<QQ', fixed, 0, 1, oid)
    struct.pack_into('<I', fixed, 68, bsd_flags)
    if not size:
        return bytes(fixed) + struct.pack('<HH', 0, 0)
    return bytes(fixed) + struct.pack('<HHBBH', 1, 40, 8, 0, 40) + struct.pack('<QQQQQ', size, 0, 0, 0, 0)


def reader(image):
    return lambda offset, length: image[offset:offset + length]


# ---------------------------------------------------------------- tests

class ContainerTests(unittest.TestCase):
    def test_the_factory_cache_is_read_from_the_hardware_volume(self):
        files = records()
        files[f'FSC2-{SENSOR}'] = FACTORY + bytes(2 * BS)  # spans blocks
        self.assertEqual(helper.Container(reader(apfs(files))).hardware(), files)

    def test_unhashed_directory_keys_are_read(self):
        files = records()
        self.assertEqual(helper.Container(reader(apfs(files, hashed=False))).hardware(), files)

    def test_unsupported_layouts_are_refused(self):
        image = apfs(records())
        for name, bad in {
            'not APFS': bytes(len(image)),
            # A byte in a tree node's free space: only the checksum notices.
            'checksum': image[:10 * BS + 2000] + b'\xff' + image[10 * BS + 2001:],
            'encrypted volume': apfs(records(), flags=0),
            'compressed record': apfs(records(), bsd_flags=0x20),
            'no Hardware volume': apfs(records(), role=0x10, extra_volume=False),
            'no factory cache': apfs(records(), path=('FactoryData',)),
        }.items():
            with self.subTest(name), self.assertRaises(helper.Refused):
                helper.Container(reader(bad)).hardware()

    def test_the_newest_valid_checkpoint_is_used(self):
        image = bytearray(apfs(records()))
        stale = bytearray(image[0:BS])
        struct.pack_into('<Q', stale, 160, 2)  # an older checkpoint's map is gone
        image[0:BS] = checksummed(bytes(stale))
        newest = bytearray(image[BS:2 * BS])
        struct.pack_into('<Q', newest, 16, 5)
        image[BS:2 * BS] = checksummed(bytes(newest))
        self.assertEqual(helper.Container(reader(bytes(image))).hardware(), records())
        image[BS + 200] ^= 1  # a damaged newest checkpoint is skipped
        with self.assertRaises(helper.Refused):
            helper.Container(reader(bytes(image))).hardware()


class ManifestTests(unittest.TestCase):
    def test_properties_are_walked_as_der(self):
        objects = helper.manifest(seal())
        self.assertEqual(objects[b'MANP'][b'SrNm'], (0x04, SERIAL))
        self.assertEqual(objects[b'FSC2'][b'DGST'], (0x04, hashlib.sha384(CALIBRATION).digest()))

    def test_digest_bytes_hidden_in_an_opaque_value_are_not_properties(self):
        hidden = text(b'DGST') + octets(hashlib.sha384(CALIBRATION).digest()) + \
            text(b'SrNm') + octets(SERIAL)
        manifest = im4m({b'MANP': {b'blob': octets(hidden)}, b'FSC2': {b'blob': octets(hidden)}})
        objects = helper.manifest(manifest)
        self.assertNotIn(b'DGST', objects[b'FSC2'])
        self.assertNotIn(b'SrNm', objects[b'MANP'])
        with self.assertRaises(helper.Refused):
            helper.admit(records(**{f'seal-{CHIP:08X}-{ECID:016X}': manifest}), IDENTITY)

    def test_duplicate_and_malformed_properties_are_refused(self):
        digest = octets(hashlib.sha384(CALIBRATION).digest())
        twice = private(b'FSC2', der(0x31, private(b'DGST', digest) + private(b'DGST', digest)))
        duplicate = der(0x30, text(b'IM4M') + der(0x02, b'\0')
                        + der(0x31, private(b'MANB', der(0x31, twice)))
                        + octets(b'sig') + der(0x30, b''))
        misnamed = seal().replace(text(b'SrNm'), text(b'SrNx'))
        long_length = bytes([0x30, 0x81, 0x05]) + seal()[2:7]
        for name, bad in {'duplicate': duplicate, 'misnamed': misnamed,
                          'non-minimal length': long_length,
                          'trailing data': seal() + b'\0',
                          'no signature': der(0x30, text(b'IM4M') + der(0x02, b'\0')
                                              + der(0x31, private(b'MANB', der(0x31, b''))))}.items():
            with self.subTest(name), self.assertRaises(helper.Refused):
                helper.manifest(bad)

    def test_records_must_be_exactly_one_element(self):
        for name, check, data in [('combined', helper.combined, EXPECTED),
                                  ('image', lambda d: helper.image(d, b'FSC2'), FACTORY)]:
            check(data)
            with self.subTest(name), self.assertRaises(helper.Refused):
                check(data + b'\0')

    def test_distinct_objects_may_each_carry_a_digest(self):
        objects = helper.manifest(seal())
        self.assertIn(b'DGST', objects[b'FSC2'])
        self.assertIn(b'DGST', objects[b'MSRk'])

    def test_der_limits(self):
        deep = b'\x04\x00'
        for _ in range(helper.DER_MAX_DEPTH + 2):
            deep = der(0x30, deep)
        with self.assertRaises(helper.Refused):
            helper.combined(deep)
        many = der(0x30, text(b'comb') + b'\x05\x00' * (helper.DER_MAX_NODES + 1))
        with self.assertRaises(helper.Refused):
            helper.combined(many)


class AdmissionTests(unittest.TestCase):
    def test_this_macs_records_rebuild_the_sealed_calibration(self):
        expected, evidence = helper.admit(records(), IDENTITY)
        self.assertEqual(expected, EXPECTED)
        self.assertEqual(evidence['trust_sha256'], hashlib.sha256(TRUST).hexdigest())
        self.assertEqual(evidence['source_sha384'], hashlib.sha384(FACTORY).hexdigest())

    def test_a_seal_whose_manb_is_a_sequence_is_admitted_unchanged(self):
        sequence = seal(container=0x30)
        self.assertEqual(helper.manifest(sequence), helper.manifest(seal()))
        expected, evidence = helper.admit(records(**{f'seal-{CHIP:08X}-{ECID:016X}': sequence}), IDENTITY)
        self.assertEqual(evidence['seal_sha256'], hashlib.sha256(sequence).hexdigest())
        self.assertIn(sequence, expected)
        with self.assertRaises(helper.Refused):
            helper.manifest(seal(container=0x04))

    def test_a_name_repeated_in_the_sequence_admits_nothing_on_it(self):
        def repeated(name, container):
            objects = [(n, dict(props)) for n, props in [
                (b'MANP', {b'SrNm': octets(SERIAL), b'BORD': integer(0x26)}),
                (b'FSC2', {b'DGST': octets(hashlib.sha384(CALIBRATION).digest()),
                           b'clas': octets(b'\x00' * 4)}),
                (b'MSRk', {b'DGST': octets(hashlib.sha384(ROOT_KEY).digest())}),
                (b'scrt', {b'DGST': octets(bytes(48))})]]
            objects.append((name, {b'DGST': octets(bytes(48))}))
            body = b''.join(private(n, der(0x31, b''.join(private(k, v) for k, v in props.items())))
                            for n, props in objects)
            return der(0x30, text(b'IM4M') + der(0x02, b'\x00') + der(0x31, private(b'MANB', der(container, body)))
                       + der(0x04, b'signature') + der(0x30, der(0x30, b'')))
        name = f'seal-{CHIP:08X}-{ECID:016X}'
        objects = helper.manifest(repeated(b'scrt', 0x30))
        self.assertEqual(objects[b'scrt'], {})
        expected, _ = helper.admit(records(**{name: repeated(b'scrt', 0x30)}), IDENTITY)
        self.assertEqual(helper.combined(expected)[1], TRUST)
        for kind in (b'FSC2', b'MANP'):
            with self.subTest(kind), self.assertRaises(helper.Refused):
                helper.admit(records(**{name: repeated(kind, 0x30)}), IDENTITY)
        with self.assertRaises(helper.Refused):
            helper.manifest(repeated(b'scrt', 0x31))

    def test_the_current_trust_bundle_is_chosen_over_an_old_one(self):
        expected, _ = helper.admit(records(), IDENTITY)
        self.assertEqual(helper.combined(expected)[1], TRUST)

    def test_trust_bundle_admission(self):
        named = f'trustobject-{hashlib.sha256(TRUST).hexdigest().upper()}'
        old = f'trustobject-{hashlib.sha256(OLD_TRUST).hexdigest().upper()}'
        for name, changes in {
            'arbitrary bytes as current': {'trustobject-current': b'not a trust bundle'},
            'current without its named copy': {named: None},
            'named copy with other bytes': {'trustobject-current': TRUST, named: OLD_TRUST},
            'no current, two named': {'trustobject-current': None},
            'no current, misnamed': {'trustobject-current': None, old: None, named: OLD_TRUST},
        }.items():
            with self.subTest(name), self.assertRaises(helper.Refused):
                helper.admit(records(**changes), IDENTITY)
        expected, _ = helper.admit(records(**{'trustobject-current': None, old: None}), IDENTITY)
        self.assertEqual(expected, EXPECTED)

    def test_identity_must_be_known_and_match(self):
        other_seal = f'seal-{CHIP:08X}-{OTHER_ECID:016X}'
        for name, (changes, identity) in {
            'unknown ECID': ({}, (CHIP, None, SERIAL)),
            'unknown serial': ({}, (CHIP, ECID, None)),
            'another ECID': ({}, (CHIP, OTHER_ECID, SERIAL)),
            "another Mac's seal": ({f'seal-{CHIP:08X}-{ECID:016X}': None, other_seal: seal()}, IDENTITY),
            'another serial': ({f'seal-{CHIP:08X}-{ECID:016X}': seal(serial=b'OTHERSERIAL')}, IDENTITY),
            'two seals': ({other_seal: seal()}, IDENTITY),
        }.items():
            with self.subTest(name), self.assertRaises(helper.Refused):
                helper.admit(records(**changes), identity)

    def test_the_seal_must_cover_this_calibration(self):
        with self.assertRaises(helper.Refused):
            helper.admit(records(**{f'seal-{CHIP:08X}-{ECID:016X}': seal(payload=OTHER)}), IDENTITY)

    def test_a_changed_calibration_or_seal_changes_the_rebuild(self):
        changed = img4(OTHER, im4m({b'MANP': {}}))
        expected, _ = helper.admit(records(**{f'FSC2-{SENSOR}': changed,
                                              f'seal-{CHIP:08X}-{ECID:016X}': seal(payload=OTHER)}),
                                   IDENTITY)
        self.assertEqual(expected, sealed(OTHER))

    def test_a_manifest_only_change_is_recorded(self):
        refreshed = img4(CALIBRATION, im4m({b'MANP': {b'srvn': octets(b'\x02')}}))
        expected, evidence = helper.admit(records(**{f'FSC2-{SENSOR}': refreshed}), IDENTITY)
        self.assertEqual(expected, EXPECTED)  # the enclave loads only the IM4P
        self.assertEqual(evidence['source_sha384'], hashlib.sha384(refreshed).hexdigest())


class IdentityTests(unittest.TestCase):
    def test_live_identity_comes_from_the_local_policy_and_the_device_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            sep = tree / 'soc' / 'sep@3aa600000'
            sep.mkdir(parents=True)
            (sep / 'compatible').write_bytes(b'apple,sep\0')
            (sep / 'local-policy-manifest').write_bytes(
                im4m({b'MANP': {b'CHIP': integer(CHIP), b'ECID': integer(ECID)}}))
            self.assertEqual(helper.live_identity(tree), (CHIP, ECID, None))
            (tree / 'serial-number').write_bytes(SERIAL + b'\0')
            self.assertEqual(helper.live_identity(tree), IDENTITY)
            (sep / 'local-policy-manifest').write_bytes(b'junk')
            self.assertEqual(helper.live_identity(tree), (None, None, SERIAL))


def device_tree(root, names=b'apple/mesacal-j713.bin\0', serial=SERIAL, sensor=True):
    """A device tree with the SEP's local policy, the serial number and,
    with `sensor`, a fingerprint node naming `names` (None: no property)."""
    tree = root / 'dt'
    sep = tree / 'soc' / 'sep@3aa600000'
    sep.mkdir(parents=True)
    (sep / 'compatible').write_bytes(b'apple,sep\0')
    (sep / 'local-policy-manifest').write_bytes(
        im4m({b'MANP': {b'CHIP': integer(CHIP), b'ECID': integer(ECID)}}))
    if serial is not None:
        (tree / 'serial-number').write_bytes(serial + b'\0')
    if sensor:
        node = tree / 'spi' / 'fingerprint@0'
        node.mkdir(parents=True)
        (node / 'compatible').write_bytes(b'apple,mesa-fingerprint\0')
        if names is not None:
            (node / 'firmware-name').write_bytes(names)
    return tree


class SensorNameTests(unittest.TestCase):
    def names(self, value, sensor=True):
        with tempfile.TemporaryDirectory() as directory:
            return helper.sensor_names(device_tree(Path(directory), value, sensor=sensor))

    def test_names_are_read_as_the_driver_reads_them(self):
        self.assertEqual(self.names(b'apple/mesacal-j700.bin\0'),
                         ['apple/mesacal-j700.bin', 'apple/msrk-j700.bin'])
        self.assertEqual(self.names(b'apple/mesacal-j616s.bin\0apple/msrk-j616s.bin\0'),
                         ['apple/mesacal-j616s.bin', 'apple/msrk-j616s.bin'])
        self.assertEqual(self.names(None), ['apple/mesa_calibration.bin', 'apple/msrk-j700.bin'])
        self.assertIsNone(self.names(b'apple/mesacal-j700.bin\0', sensor=False))

    def test_unsafe_names_are_refused(self):
        for value in (b'apple/../../escape.bin\0', b'apple/mesacal-j616s.bin\0/etc/shadow\0',
                      b'firmware.bin\0'):
            with self.subTest(value), self.assertRaises(ValueError):
                self.names(value)

    def test_a_disabled_node_or_controller_is_no_sensor(self):
        # Mac mini and Mac Studio keep the SoC's fingerprint node disabled, and
        # the boot loader disables the controller on a J700 without a reader.
        for where, value in (('node', b'disabled\0'), ('controller', b'disabled\0'),
                             ('node', b'fail\0')):
            with self.subTest(where=where, value=value), tempfile.TemporaryDirectory() as directory:
                tree = device_tree(Path(directory), b'apple/mesacal-j700.bin\0')
                node = tree / 'spi' / 'fingerprint@0'
                ((node if where == 'node' else node.parent) / 'status').write_bytes(value)
                self.assertIsNone(helper.sensor_names(tree))
        for value in (b'okay\0', b'ok\0'):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                tree = device_tree(Path(directory), b'apple/mesacal-j700.bin\0')
                (tree / 'spi' / 'fingerprint@0' / 'status').write_bytes(value)
                (tree / 'spi' / 'status').write_bytes(value)
                self.assertEqual(helper.sensor_names(tree)[0], 'apple/mesacal-j700.bin')

    def test_a_disabled_second_node_does_not_conflict(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = device_tree(Path(directory))
            second = tree / 'spi2' / 'fingerprint@0'
            second.mkdir(parents=True)
            (second / 'compatible').write_bytes(b'apple,mesa-fingerprint\0')
            (second / 'firmware-name').write_bytes(b'apple/other.bin\0')
            (second / 'status').write_bytes(b'disabled\0')
            self.assertEqual(helper.sensor_names(tree)[0], 'apple/mesacal-j713.bin')

    def test_conflicting_nodes_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = device_tree(Path(directory))
            second = tree / 'spi2' / 'fingerprint@0'
            second.mkdir(parents=True)
            (second / 'compatible').write_bytes(b'apple,mesa-fingerprint\0')
            (second / 'firmware-name').write_bytes(b'apple/other.bin\0')
            with self.assertRaises(ValueError):
                helper.sensor_names(tree)


class SetupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.state, self.run = self.root / 'state', self.root / 'run'
        self.firmware = self.root / 'firmware'
        self.target = self.firmware / 'apple' / 'mesacal-j713.bin'
        self.root_key = self.firmware / 'apple' / 'msrk-j700.bin'
        self.carved = self.root / 'carved.bin'
        self.extractor = self.root / 'extract'
        self.extractor.write_text('#!/bin/sh\n[ -f "$CARVED" ] || { echo "error: nothing" >&2; exit 1; }\n'
                                  'cp "$CARVED" "$3" && chmod 600 "$3"\n')
        self.extractor.chmod(0o755)
        os.environ['CARVED'] = str(self.carved)
        self.addCleanup(os.environ.pop, 'CARVED', None)
        self.image = apfs(records())
        self.tree = device_tree(self.root)

    def setup(self, **options):
        options.setdefault('sep_bio', self.root / 'no-sep-bio')
        options.setdefault('devices', self.root / 'no-devices')
        with contextlib.redirect_stderr(io.StringIO()) as out:
            result = helper.setup(reader(self.image), self.root / 'device', self.extractor,
                                  self.tree, self.firmware, self.state, self.run, **options)
        self.output = out.getvalue()
        return result

    def install(self, path, data, mode=0o600):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)

    def actions(self, result):
        return {item['role']: item['action'] for item in result['files']}

    def rollback(self):
        directory = self.state / 'rollback'
        return sorted(p.read_bytes() for p in directory.iterdir()) if directory.exists() else []

    def test_missing_files_are_built_and_installed(self):
        result = self.setup()
        self.assertEqual(result['status'], 'changed')
        self.assertEqual(self.actions(result), {'calibration': 'install', 'root key': 'install'})
        self.assertEqual(self.target.read_bytes(), EXPECTED)
        self.assertEqual(self.root_key.read_bytes(), EXPECTED_ROOT_KEY)
        self.assertEqual(self.target.stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.state / helper.FILES).read_text().split(),
                         [str(self.target), str(self.root_key)])
        recorded = json.loads((self.state / helper.RESULT).read_text())
        self.assertEqual(recorded['files'][0]['sha256'], hashlib.sha256(EXPECTED).hexdigest())
        self.assertEqual(len((self.state / helper.HISTORY).read_text().splitlines()), 1)
        self.assertEqual(self.rollback(), [])

    def test_check_writes_nothing(self):
        result = self.setup(check=True)
        self.assertEqual(result['status'], 'would-change')
        self.assertEqual(self.actions(result), {'calibration': 'install', 'root key': 'install'})
        self.assertFalse(self.firmware.exists())
        self.assertFalse(self.state.exists())

    def test_identical_files_are_kept_untouched(self):
        self.install(self.target, EXPECTED, 0o644)
        self.install(self.root_key, EXPECTED_ROOT_KEY)
        before = self.target.stat()
        result = self.setup()
        self.assertEqual(result['status'], 'ok')
        after = self.target.stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns, after.st_mode),
                         (before.st_ino, before.st_mtime_ns, before.st_mode))
        self.assertEqual(self.setup(check=True)['status'], 'ok')

    def test_a_file_this_tool_did_not_install_is_left(self):
        foreign = sealed(trust=OLD_TRUST)
        self.install(self.target, foreign)
        self.install(self.root_key, EXPECTED_ROOT_KEY)
        result = self.setup()
        self.assertEqual(self.actions(result)['calibration'], 'leave')
        self.assertEqual(result['status'], 'left')
        self.assertEqual(self.target.read_bytes(), foreign)
        self.assertIn('--force-rebuild', self.output)

    def test_force_rebuild_and_a_refusal_replace_it_with_a_rollback_copy(self):
        for trigger in ('force', 'refused'):
            with self.subTest(trigger):
                foreign = sealed(trust=OLD_TRUST) + trigger.encode()
                self.install(self.target, foreign)
                result = self.setup(trigger=trigger)
                self.assertEqual(self.actions(result)['calibration'], 'replace')
                self.assertEqual(self.target.read_bytes(), EXPECTED)
                self.assertIn(foreign, self.rollback())
                self.assertEqual(Path(result['files'][0]['rollback']).read_bytes(), foreign)

    def test_a_refused_file_identical_to_the_rebuild_is_kept_and_explained(self):
        self.install(self.target, EXPECTED)
        result = self.setup(trigger='refused')
        self.assertEqual(self.actions(result)['calibration'], 'keep')
        self.assertIn('cannot fix the refusal', result['files'][0]['reason'])

    def test_its_own_install_follows_changed_factory_data(self):
        self.setup()
        self.image = apfs(records(**{f'FSC2-{SENSOR}': img4(OTHER, im4m({b'MANP': {}})),
                                     f'seal-{CHIP:08X}-{ECID:016X}': seal(payload=OTHER)}))
        result = self.setup()
        self.assertEqual(self.actions(result)['calibration'], 'replace')
        self.assertEqual(self.target.read_bytes(), sealed(OTHER))
        self.assertIn(EXPECTED, self.rollback())

    def test_the_old_helpers_record_and_cache_count_as_ours(self):
        for name in ('installed.sha256', 'mesa-calibration.bin'):
            with self.subTest(name):
                old = b'what prepare-sep-calibration installed ' + name.encode()
                self.install(self.target, old)
                helper.private_directory(self.state)
                legacy = self.state / name
                self.install(legacy, (hashlib.sha256(old).hexdigest() + '\n').encode()
                             if name.endswith('.sha256') else old)
                self.assertEqual(self.actions(self.setup())['calibration'], 'replace')
                self.assertEqual(self.target.read_bytes(), EXPECTED)
                legacy.unlink()
                (self.state / helper.RESULT).unlink()

    def test_the_bare_image_is_never_installed(self):
        self.image = bytes(len(self.image))
        self.carved.write_bytes(FACTORY)
        result = self.setup()
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(self.actions(result), {'calibration': 'fail'})
        self.assertIn('the boot container is not APFS', result['files'][0]['reason'])
        self.assertFalse(self.target.exists())

    def test_an_fscl_record_is_installed_as_carved(self):
        self.image = apfs({'HmCA-0': b'unrelated'})
        for fscl in FSCL_RECORDS:
            with self.subTest(len(fscl)):
                self.carved.write_bytes(fscl)
                self.target.unlink(missing_ok=True)
                result = self.setup()
                self.assertEqual(self.actions(result), {'calibration': 'install'})
                self.assertEqual(self.target.read_bytes(), fscl)
                self.assertEqual(result['files'][0]['source'], 'carved from the iBoot System Container')

    def test_no_carvable_record_fails_without_writing(self):
        self.image = apfs({'HmCA-0': b'unrelated'})
        result = self.setup()
        self.assertEqual(result['status'], 'failed')
        self.assertIn('the extractor found no calibration (error: nothing)', result['files'][0]['reason'])
        self.assertFalse(self.target.exists())

    def test_a_mac_without_a_reader_records_that_once(self):
        self.tree = device_tree(self.root / 'sensorless', sensor=False)
        result = self.setup()
        self.assertEqual(result['status'], 'no-sensor')
        self.assertEqual((self.state / helper.FILES).read_text(), '')
        self.assertFalse(self.firmware.exists())

    def test_unknown_identity_fails_and_keeps_the_installed_file(self):
        self.tree = device_tree(self.root / 'noserial', serial=None)
        self.install(self.target, EXPECTED)
        result = self.setup(trigger='refused')
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(self.target.read_bytes(), EXPECTED)

    def test_a_link_at_the_target_is_not_followed(self):
        outside = self.root / 'outside'
        outside.write_bytes(b'keep')
        self.target.parent.mkdir(parents=True)
        self.target.symlink_to(outside)
        self.assertEqual(self.actions(self.setup())['calibration'], 'leave')
        self.assertTrue(self.target.is_symlink())
        self.assertEqual(self.actions(self.setup(trigger='force'))['calibration'], 'replace')
        self.assertFalse(self.target.is_symlink())
        self.assertEqual(self.target.read_bytes(), EXPECTED)
        self.assertEqual(outside.read_bytes(), b'keep')

    def test_a_failed_replacement_leaves_the_old_file(self):
        old = sealed(trust=OLD_TRUST)
        self.install(self.target, old)
        real = helper.atomic_write

        def fail_on_target(path, data, mode=0o600):
            if path == self.target:
                raise OSError('disk full')
            real(path, data, mode)

        with patch.object(helper, 'atomic_write', side_effect=fail_on_target):
            with self.assertRaises(OSError):
                self.setup(trigger='force')
        self.assertEqual(self.target.read_bytes(), old)
        self.assertEqual(self.rollback(), [old])

    def test_state_and_firmware_links_are_refused(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        self.state.symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.setup()
        self.state.unlink()
        self.firmware.mkdir()
        (self.firmware / 'apple').symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.setup()
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_files_kept_elsewhere_are_linked_into_the_search_path(self):
        search = self.root / 'run-firmware'
        result = self.setup(search=search)
        link = search / 'apple' / 'mesacal-j713.bin'
        self.assertEqual(os.readlink(link), str(self.target))
        self.assertEqual(result['files'][0]['published'], str(link))
        self.setup(search=search)
        self.assertEqual(link.read_bytes(), EXPECTED)

    def test_files_are_kept_on_persistent_storage(self):
        # Arch and NixOS point the kernel's search path at the vendor firmware
        # unpacked on /run each boot: keep the files elsewhere and link them.
        path = self.root / 'firmware_class.path'
        cases = (('', None, '/usr/lib/firmware', None),
                 ('/run/aurora-firmware/vendorfw\n', None, '/usr/lib/firmware',
                  '/run/aurora-firmware/vendorfw'),
                 ('/run/aurora-firmware/vendorfw', '/var/lib/aurora-sep/firmware',
                  '/var/lib/aurora-sep/firmware', '/run/aurora-firmware/vendorfw'),
                 ('/usr/lib/firmware', None, '/usr/lib/firmware', None))
        for value, kept, firmware, search in cases:
            with self.subTest(value=value, kept=kept):
                path.write_text(value)
                env = {'AURORA_TOUCHID_FIRMWARE_DIR': kept} if kept else {}
                with patch.object(helper, 'FIRMWARE_PATH', path), \
                        patch.dict(os.environ, env, clear=False):
                    if not kept:
                        os.environ.pop('AURORA_TOUCHID_FIRMWARE_DIR', None)
                    self.assertEqual(helper.firmware_dirs(),
                                     (Path(firmware), Path(search) if search else None))
        with patch.object(helper, 'FIRMWARE_PATH', self.root / 'absent'):
            os.environ.pop('AURORA_TOUCHID_FIRMWARE_DIR', None)
            self.assertEqual(helper.firmware_dirs(), (Path('/usr/lib/firmware'), None))

    def test_next_steps_follow_the_driver(self):
        devices = self.root / 'devices'
        diag = devices / '3aa600000.sep' / 'diag'
        diag.mkdir(parents=True)
        (diag / 'sensor_attach').write_text('not-ready\n')
        sep_bio = self.root / 'sep-bio'
        sep_bio.touch()
        result = self.setup(sep_bio=sep_bio, devices=devices)
        self.assertIn('next sensor bring-up', ' '.join(result['next_steps']))
        (diag / 'sensor_attach').write_text('attached\n')
        (diag / 'restore').write_text('restored\n')
        result = self.setup(sep_bio=sep_bio, devices=devices)
        self.assertEqual(result['next_steps'], [
            'Nothing to do: the files are in place and the enrolled fingerprints are restored.'])


class TriggerTests(unittest.TestCase):
    NEEDED = PACKAGE / 'touchid-setup-needed'

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.run, self.files = self.root / 'run', self.root / 'touchid-files'
        self.run.mkdir()

    def needed(self):
        return subprocess.run(['sh', str(self.NEEDED)], env={
            **os.environ, 'AURORA_SEP_RUN': str(self.run),
            'AURORA_TOUCHID_FILES': str(self.files)}).returncode == 0

    def test_the_boot_condition_only_checks_that_files_exist(self):
        self.assertTrue(self.needed())  # never ran on this Mac
        present = self.root / 'mesacal.bin'
        present.write_bytes(b'x')
        self.files.write_text(f'{present}\n')
        self.assertFalse(self.needed())
        self.files.write_text('')  # a Mac without a reader
        self.assertFalse(self.needed())
        self.files.write_text(f'{present}\n{self.root / "msrk.bin"}\n')
        self.assertTrue(self.needed())

    def test_a_refusal_runs_it_once_per_boot(self):
        self.files.write_text('')
        (self.run / helper.REFUSED).write_text('sensor: LOAD_CALIBRATION refused with status 0x1c\n')
        self.assertTrue(self.needed())
        state = self.root / 'state'
        self.assertEqual(helper.automatic(self.run, state), 'refused')
        self.assertFalse(self.needed())
        self.assertIsNone(helper.automatic(self.run, state))

    def test_without_a_refusal_the_trigger_is_a_missing_file(self):
        self.assertEqual(helper.automatic(self.run, self.root / 'state'), 'missing')
        self.assertFalse((self.run / helper.AUTO_DONE).exists())


class ReadyRepairTests(unittest.TestCase):
    """aurora-sep-ready starts the rebuild only when the sensor did not attach
    and the kernel log shows a refusal, and only once per boot."""
    READY = PACKAGE / 'aurora-sep-ready'
    REFUSAL = 'apple_sep 282600000.sep: sensor: LOAD_CALIBRATION refused with status 0x1c'

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bin, self.run = self.root / 'bin', self.root / 'run'
        self.bin.mkdir()
        self.diag = self.root / 'devices' / '282600000.sep' / 'diag'
        self.diag.mkdir(parents=True)
        self.calls = self.root / 'calls'
        for tool in ('journalctl', 'systemctl', 'authselect'):
            (self.bin / tool).write_text(f'#!/bin/sh\necho "{tool} $*" >>"{self.calls}"\n'
                                         + ('cat "$KMSG"\n' if tool == 'journalctl' else ''))
            (self.bin / tool).chmod(0o755)
        self.kmsg = self.root / 'kmsg'

    def ready(self, attach, kmsg=''):
        for name, value in {'touchid': 'ready', 'sensor_attach': attach,
                            'restore': 'restored', 'matching': 'usable', 'sensor': 'online'}.items():
            (self.diag / name).write_text(value + '\n')
        self.kmsg.write_text(kmsg)
        done = subprocess.run(['sh', str(self.READY)], capture_output=True, text=True, env={
            **os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}', 'KMSG': str(self.kmsg),
            'AURORA_SEP_DEV': '/dev/null', 'AURORA_SEP_SYSFS': str(self.root / 'devices'),
            'AURORA_SEP_READY_WAIT': '0', 'AURORA_SEP_RUN': str(self.run)})
        calls = self.calls.read_text() if self.calls.exists() else ''
        self.calls.unlink(missing_ok=True)
        return done.returncode, calls, done.stderr

    def test_an_attached_sensor_never_reads_the_log(self):
        status, calls, _ = self.ready('attached', self.REFUSAL)
        self.assertEqual(status, 0)
        self.assertNotIn('journalctl', calls)

    def test_a_refusal_starts_the_rebuild_once(self):
        status, calls, err = self.ready('not-ready', self.REFUSAL + '\n')
        self.assertEqual(status, 1)
        self.assertIn('systemctl start --no-block aurora-touchid-setup.service', calls)
        self.assertIn('LOAD_CALIBRATION refused', (self.run / 'calibration-refused').read_text())
        self.assertIn('A rebuild of the Touch ID file was started', err)
        (self.run / 'touchid-auto-rebuild').write_text('boot\n')
        status, calls, err = self.ready('not-ready', self.REFUSAL + '\n')
        self.assertNotIn('systemctl', calls)
        self.assertIn('already ran this boot', err)

    def test_an_attach_failure_without_a_refusal_starts_nothing(self):
        status, calls, _ = self.ready('not-ready', 'apple_sep: sensor: the power cycle failed\n')
        self.assertEqual(status, 1)
        self.assertIn('journalctl', calls)
        self.assertNotIn('systemctl', calls)
        self.assertFalse((self.run / 'calibration-refused').exists())


if __name__ == '__main__':
    unittest.main()
