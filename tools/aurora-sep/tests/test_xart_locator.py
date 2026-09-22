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


class RawLocatorTests(unittest.TestCase):
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
