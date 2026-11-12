# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic tests for the exact-ID Linux-lane recovery utility."""

import runpy
import struct
import unittest
import uuid
from pathlib import Path
from unittest.mock import patch


CLI = runpy.run_path(str(Path(__file__).parents[1] / "sep-bio-identities"), run_name="sep_bio_test")


class IdentityToolTests(unittest.TestCase):
    def test_list_decodes_only_counted_uuids(self):
        a = uuid.UUID(int=1)
        b = uuid.UUID(int=2)

        def fake_ioctl(_fd, request, reply, _mutate):
            self.assertEqual(request, CLI["LIST_ENCLAVE"])
            struct.pack_into("=II", reply, 0, 2, 0)
            reply[8:24] = a.bytes
            reply[24:40] = b.bytes

        with patch.object(CLI["fcntl"], "ioctl", fake_ioctl):
            self.assertEqual(CLI["list_identities"](7), [a, b])

    def test_exact_target_and_keep_are_required(self):
        a = uuid.UUID(int=1)
        b = uuid.UUID(int=2)
        self.assertIsNone(CLI["check_target"]([a, b], a, [b]))
        for before, target, keep in [
            ([a, b], a, []),
            ([a, b], a, [a]),
            ([a, b], uuid.UUID(int=3), [b]),
            ([a, b], a, [uuid.UUID(int=3)]),
        ]:
            with self.assertRaises(ValueError):
                CLI["check_target"](before, target, keep)


if __name__ == "__main__":
    unittest.main()
