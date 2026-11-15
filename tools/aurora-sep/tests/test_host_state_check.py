# SPDX-License-Identifier: MIT
"""The read-only host-state check agrees with the kernel driver's rules."""

import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "host-state-check.py"
SPEC = importlib.util.spec_from_file_location("host_state_check", SCRIPT)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)

BLOCK = CHECK.BLOCK_SIZE


class Writer:
    """The driver's writer: copy-on-write into the first free slot."""

    def __init__(self):
        self.image = bytearray(CHECK.STORE_SIZE)
        self.slots = [None] * CHECK.SLOT_COUNT
        self.generation = 1
        self.superblock()

    def superblock(self, intent=(0, 0, 0)):
        sb = bytearray(BLOCK)
        sb[:16] = CHECK.MAGIC
        struct.pack_into("<IIIQ", sb, 16, 1, CHECK.SLOT_COUNT, BLOCK,
                         self.generation)
        sb[36] = intent[0]
        struct.pack_into("<H", sb, 37, intent[1])
        sb[39] = intent[2]
        for i, slot in enumerate(self.slots):
            if slot:
                off = CHECK.SB_SLOT_TABLE + i * CHECK.SLOT_ENTRY_SIZE
                kind, uuid, length = slot
                sb[off] = 1
                sb[off + 1] = kind
                sb[off + 2:off + 18] = uuid
                struct.pack_into("<I", sb, off + 18, length)
        self.image[:BLOCK] = sb

    def write(self, kind, value, uuid=bytes(16), commit=True):
        old = next((i for i, s in enumerate(self.slots)
                    if s and s[:2] == (kind, uuid)), None)
        fresh = self.slots.index(None)
        self.superblock((1, fresh, kind))
        self.image[(fresh + 1) * BLOCK:(fresh + 1) * BLOCK + len(value)] = value
        if not commit:
            return
        self.slots[fresh] = (kind, uuid, len(value))
        if old is not None:
            self.slots[old] = None
        self.generation += 1
        self.superblock()

    def poke(self, off, data):
        self.image[off:off + len(data)] = data
        return self

    def verdict(self, size=None):
        size = len(self.image) if size is None else size
        return CHECK.damage(size, bytes(self.image[:BLOCK]))


def entry(slot, byte):
    return CHECK.SB_SLOT_TABLE + slot * CHECK.SLOT_ENTRY_SIZE + byte


def two_records():
    """Slot 0 holds 0xf1, slot 1 an empty 0xf5, slot 2 is free."""
    w = Writer()
    w.write(0xF1, b"index")
    w.write(0xF5, b"")
    return w


class Accepts(unittest.TestCase):
    def test_new_store(self):
        self.assertIsNone(Writer().verdict())

    def test_records_overwrites_and_keys(self):
        w = Writer()
        w.write(0xF1, b"a" * 300)
        w.write(0xF2, b"b" * BLOCK)
        w.write(0xF0, b"c" * 16, uuid=b"\x5c" * 16)
        w.write(0xF1, b"d" * 600)
        w.write(0xF5, b"")
        self.assertIsNone(w.verdict())

    def test_interrupted_write(self):
        w = two_records()
        w.write(0xF1, b"new", commit=False)
        self.assertIsNone(w.verdict())


class Refuses(unittest.TestCase):
    def test_size(self):
        for size in (0, 1, BLOCK, CHECK.STORE_SIZE - 1, CHECK.STORE_SIZE + 1):
            self.assertEqual(Writer().verdict(size), "Size")

    def test_header(self):
        self.assertEqual(Writer().poke(0, b"X").verdict(), "Header")
        for off, value in ((16, 0), (16, 2), (20, CHECK.SLOT_COUNT - 1),
                           (24, BLOCK // 2)):
            w = Writer().poke(off, struct.pack("<I", value))
            self.assertEqual(w.verdict(), "Header")
        zero = Writer()
        zero.image[:] = bytes(len(zero.image))
        self.assertEqual(zero.verdict(), "Header")

    def test_reserved(self):
        for off in (40, 63, CHECK.SB_SLOT_TABLE_END, BLOCK - 1):
            self.assertEqual(Writer().poke(off, b"\1").verdict(), "Reserved")

    def test_slots(self):
        for off, data, slot in ((entry(2, 1), b"\xf0", 2),
                                (entry(2, 18), b"\1", 2),
                                (entry(0, 0), b"\2", 0),
                                (entry(0, 1), b"\xef", 0),
                                (entry(0, 1), b"\xf6", 0),
                                (entry(1, 22), b"\1", 1),
                                (entry(1, 18), struct.pack("<I", BLOCK + 1), 1),
                                (entry(1, 18), b"\xff" * 4, 1)):
            w = two_records().poke(off, data)
            self.assertEqual(w.verdict(), f"Slot({slot})")

    def test_duplicate(self):
        w = Writer()
        w.write(0xF1, b"a")
        w.write(0xF1, b"b", uuid=b"\7" * 16)
        self.assertIsNone(w.verdict())
        w.poke(entry(1, 2), bytes(16))
        self.assertEqual(w.verdict(), "Duplicate(1)")

    def test_intent(self):
        for kind, slot, record_kind in ((1, 0, 0xF2), (1, CHECK.SLOT_COUNT, 0xF2),
                                        (1, 2, 0xEF), (2, 2, 0xF2),
                                        (0, 2, 0), (0, 0, 0xF2)):
            w = two_records().poke(36, bytes([kind]))
            w.poke(37, struct.pack("<H", slot)).poke(39, bytes([record_kind]))
            self.assertEqual(w.verdict(), "Intent")


class Command(unittest.TestCase):
    def run_check(self, image):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "store")
            with open(path, "wb") as f:
                f.write(image)
            os.chmod(path, 0o400)
            before = hashlib.sha256(image).digest()
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = CHECK.main([path])
            with open(path, "rb") as f:
                self.assertEqual(hashlib.sha256(f.read()).digest(), before)
            return status, out.getvalue()

    def test_reports_only_the_verdict(self):
        w = Writer()
        w.write(0xF1, b"secret-record-value")
        self.assertEqual(self.run_check(bytes(w.image)), (0, "ok: 1 record\n"))
        w.poke(entry(0, 1), b"\xef")
        status, out = self.run_check(bytes(w.image))
        self.assertEqual((status, out), (1, "refused: Slot(0)\n"))
        self.assertNotIn("secret", out)

    def test_counts_records(self):
        self.assertEqual(self.run_check(bytes(Writer().image)),
                         (0, "ok: 0 records\n"))

    def test_pending_write(self):
        w = two_records()
        w.write(0xF1, b"new", commit=False)
        self.assertEqual(
            self.run_check(bytes(w.image)),
            (0, "ok: 2 records, an interrupted write will be discarded\n"))

    def test_describes_what_a_refused_file_holds(self):
        # An interrupted creation leaves zeros: an empty file, a run of zero
        # blocks, or every block but no superblock.
        for image, verdict in ((b"", "Size, empty"),
                               (bytes(1), "Size, all zero"),
                               (bytes(5 * BLOCK + 1), "Size, all zero"),
                               (bytes(CHECK.STORE_SIZE), "Header, all zero"),
                               # Creation never writes past the store.
                               (bytes(CHECK.STORE_SIZE + 1), "Size, no header"),
                               (b"\1", "Size, no header")):
            self.assertEqual(self.run_check(image), (1, f"refused: {verdict}\n"))

        # A truncated or extended store keeps its header and records.
        w = two_records()
        w.write(0xF2, b"secret-record-value")
        for image, records in ((w.image[:2 * BLOCK], "3 records"),
                               (w.image + b"\0", "3 records"),
                               # Only the first entry survives.
                               (w.image[:entry(1, 0)], "1 record")):
            status, out = self.run_check(bytes(image))
            self.assertEqual(
                (status, out), (1, f"refused: Size, header present, {records}\n"))
            self.assertNotIn("secret", out)

        # A single byte of data anywhere is not all zero.
        image = bytearray(CHECK.STORE_SIZE)
        image[-1] = 1
        self.assertEqual(self.run_check(bytes(image)), (1, "refused: Header\n"))

    def test_unreadable(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(CHECK.main(["/nonexistent/store"]), 2)


if __name__ == "__main__":
    unittest.main()
