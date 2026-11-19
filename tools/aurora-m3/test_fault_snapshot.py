"""Offline fault-evidence checks; never touches a GPU or watchdog."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from fault_snapshot import collect, decode, MAGIC, MAX_BYTES


def snapshot(created=100):
    payload = b'\x04\x00\x00\x00'
    raw = MAGIC + struct.pack('<IIQQQ', 1, 1, 0, created, 108)
    raw += b'event-ring'.ljust(32, b'\0') + struct.pack('<4Q', 0x1234000, 4, created+1, created+2)
    return raw + payload


class SnapshotTests(unittest.TestCase):
    def test_decode(self):
        raw = snapshot()
        result = decode(raw)
        self.assertEqual(result['sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(result['records'][0]['name'], 'event-ring')
        self.assertEqual(result['records'][0]['offset'], 104)

    def test_rejects_bad_envelopes(self):
        for raw in (b'', snapshot()[:-1], snapshot()+b'X',
                    b'M4FWD001'+snapshot()[8:], snapshot()[:12]+b'\0'*4+snapshot()[16:]):
            with self.subTest(raw=raw[:40]), self.assertRaises(ValueError):
                decode(raw)

    def test_rejects_record_bounds_and_intervals(self):
        for offset, value in ((80, 999999), (88, 99), (96, 100)):
            raw = bytearray(snapshot())
            struct.pack_into('<Q', raw, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decode(raw)

    def test_collection_preserves_source_and_filters_stale(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)/'sysfs'; output = Path(d)/'evidence'; output.mkdir()
            for i, raw in enumerate((snapshot(10), snapshot(100), b'OTHERDMP')):
                p = root/f'devcd{i}'/'data'; p.parent.mkdir(parents=True); p.write_bytes(raw)
            reports = collect(output, 50, root)
            self.assertEqual(len(reports), 1)
            self.assertEqual((output/reports[0]['file']).read_bytes(), snapshot(100))
            self.assertEqual((root/'devcd1/data').read_bytes(), snapshot(100))
            self.assertEqual(len(list(output.iterdir())), 2)

    def test_collection_rejects_oversize_before_reading_payload(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); p = root/'devcd0/data'; p.parent.mkdir()
            p.write_bytes(MAGIC + struct.pack('<IIQQQ', 1, 1, 0, 100, MAX_BYTES+1))
            with self.assertRaisesRegex(ValueError, 'bound'):
                collect(root, 0, root)

if __name__ == '__main__': unittest.main()
