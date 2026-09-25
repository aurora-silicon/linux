"""Regression proof for the ambiguity of the legacy raw-header locator."""

import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zlib


SCRIPT = Path(__file__).resolve().parents[1] / "apfs-xart-inspect.py"
SPEC = importlib.util.spec_from_file_location("apfs_xart_inspect", SCRIPT)
INSPECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECTOR)


def record(kind, revision):
    out = bytearray(0x9000)
    payload = b"test"
    out[1] = kind
    struct.pack_into("<I", out, 0x12, len(payload))
    struct.pack_into("<I", out, 0x16, zlib.crc32(payload))
    struct.pack_into("<Q", out, 0x1a, revision)
    out[0x22:0x26] = payload
    return out


def extent_reference(start, blocks, owner, refcount=1, kind=1):
    key = struct.pack("<Q", (2 << 60) | start)
    value = struct.pack("<QQi", (kind << 60) | blocks, owner, refcount)
    return key, value


class ExtentReferenceTests(unittest.TestCase):
    def test_unshared_whole_file_reference(self):
        expected = (1236, 1536, 1, 16, 1)
        self.assertEqual(
            INSPECTOR.validate_extent_references(
                [extent_reference(1236, 1536, 16)], 1236, 1536, 16),
            expected)

    def test_shared_or_mismatched_reference_fails_closed(self):
        cases = [
            [],
            [extent_reference(1236, 1536, 16, refcount=2)],
            [extent_reference(1236, 1536, 17)],
            [extent_reference(1236, 1536, 16, kind=2)],
            [extent_reference(1237, 1535, 16)],
            [extent_reference(1235, 1537, 16)],
            [extent_reference(1236, 1536, 16)] * 2,
        ]
        for entries in cases:
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                INSPECTOR.validate_extent_references(entries, 1236, 1536, 16)


class RawLocatorTests(unittest.TestCase):
    def test_gigalocker_filename_forms(self):
        self.assertTrue(INSPECTOR.is_gigalocker_name(b".gl"))
        self.assertTrue(INSPECTOR.is_gigalocker_name(
            b"242F8E43-7258-507F-AEEC-821F00290A1F.gl"))
        self.assertTrue(INSPECTOR.is_gigalocker_name(
            b"242f8e43-7258-507f-aeec-821f00290a1f.gl"))
        self.assertFalse(INSPECTOR.is_gigalocker_name(b"242F8E43.gl"))
        self.assertFalse(INSPECTOR.is_gigalocker_name(
            b"242F8E43-7258-507F-AEEC-821F00290A1G.gl"))
        self.assertFalse(INSPECTOR.is_gigalocker_name(b"other.gl"))

    def test_roots_moved_forward_pick_window_outside_real_extent(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic-container"
            with path.open("wb") as stream:
                stream.truncate(12 * 1024 * 1024)
            true_block = 100
            true_base = true_block * 4096
            with path.open("r+b") as stream:
                for slot, kind in ((10, 1), (20, 2), (21, 4)):
                    stream.seek(true_base + slot * 0x9000)
                    stream.write(record(kind, slot))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                INSPECTOR.inspect_raw_locator(INSPECTOR.Disk(path), true_block)
            self.assertIn("shift_blocks=90", output.getvalue())


if __name__ == "__main__":
    unittest.main()
