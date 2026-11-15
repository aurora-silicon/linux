#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check a SEP host-state store the way the kernel driver does, read-only.

Prints "ok" with the number of records, or the reason the driver would refuse
the file in the words of its kernel log line, and never any record contents.
A file refused for its size or header is also described as empty, all zero,
holding a header and N records, or holding no header, so that what an
interrupted creation leaves can be told from a damaged store. Exit status 0
means the driver accepts the file, 1 that it refuses it, 2 that it could not be
read.
"""

import argparse
import os
import struct
import sys

BLOCK_SIZE = 0x8000
BLOCK_COUNT = 72
STORE_SIZE = BLOCK_SIZE * BLOCK_COUNT
SLOT_COUNT = BLOCK_COUNT - 1
MAX_VALUE = BLOCK_SIZE
MAGIC = b"AURORA-SEP-STOR\x01"
VERSION = 1
SB_INTENT_KIND = 36
SB_INTENT_SLOT = 37
SB_INTENT_TYPE = 39
SB_RESERVED = 40
SB_SLOT_TABLE = 64
SLOT_ENTRY_SIZE = 24
SB_SLOT_TABLE_END = SB_SLOT_TABLE + SLOT_COUNT * SLOT_ENTRY_SIZE
INTENT_NONE = 0
INTENT_WRITE = 1
KINDS = range(0xF0, 0xF6)


def has_header(sb):
    """Whether sb starts with this format's header."""
    if len(sb) < 28:
        return False
    version, slots, block = struct.unpack_from("<III", sb, 16)
    return sb[:16] == MAGIC and version == VERSION and slots == SLOT_COUNT \
        and block == BLOCK_SIZE


def is_zero(f, size):
    """Whether the file holds nothing but zeros. Like the driver, look no
    further than a store's worth: creation never writes past it."""
    if size > STORE_SIZE:
        return False
    f.seek(0)
    left = size
    while left:
        chunk = f.read(min(left, BLOCK_SIZE))
        if not chunk or any(chunk):
            return False
        left -= len(chunk)
    return True


def records(n):
    return f"{n} record" + ("" if n == 1 else "s")


def describe(f, size, sb, reason):
    """Describe a file refused for its size or header, without its contents."""
    if size == 0:
        return "empty"
    if is_zero(f, size):
        return "all zero"
    if reason != "Size":
        return None
    if not has_header(sb):
        return "no header"
    used = sum(1 for i in range(SLOT_COUNT)
               if SB_SLOT_TABLE + i * SLOT_ENTRY_SIZE < len(sb)
               and sb[SB_SLOT_TABLE + i * SLOT_ENTRY_SIZE])
    return f"header present, {records(used)}"


def damage(size, sb):
    """Return None if the driver accepts the store, else why it refuses it."""
    if size != STORE_SIZE or len(sb) != BLOCK_SIZE:
        return "Size"
    if not has_header(sb):
        return "Header"
    if any(sb[SB_RESERVED:SB_SLOT_TABLE]) or any(sb[SB_SLOT_TABLE_END:]):
        return "Reserved"

    keys = set()
    free = []
    for i in range(SLOT_COUNT):
        off = SB_SLOT_TABLE + i * SLOT_ENTRY_SIZE
        entry = sb[off:off + SLOT_ENTRY_SIZE]
        if not any(entry):
            free.append(True)
            continue
        length = struct.unpack_from("<I", entry, 18)[0]
        if entry[0] != 1 or entry[1] not in KINDS or length > MAX_VALUE \
                or any(entry[22:]):
            return f"Slot({i})"
        key = bytes(entry[1:18])
        if key in keys:
            return f"Duplicate({i})"
        keys.add(key)
        free.append(False)

    kind = sb[SB_INTENT_KIND]
    slot = struct.unpack_from("<H", sb, SB_INTENT_SLOT)[0]
    record_kind = sb[SB_INTENT_TYPE]
    if kind == INTENT_NONE:
        valid = slot == 0 and record_kind == 0
    elif kind == INTENT_WRITE:
        valid = slot < SLOT_COUNT and free[slot] and record_kind in KINDS
    else:
        valid = False
    return None if valid else "Intent"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("store", help="the store, or a private copy of it")
    args = parser.parse_args(argv)
    try:
        with open(args.store, "rb") as f:
            size = os.fstat(f.fileno()).st_size
            sb = f.read(BLOCK_SIZE)
            reason = damage(size, sb)
            if reason in ("Size", "Header"):
                what = describe(f, size, sb, reason)
                if what:
                    reason += f", {what}"
    except OSError as e:
        print(f"unreadable: {e.strerror}", file=sys.stderr)
        return 2

    if reason:
        print(f"refused: {reason}")
        return 1
    used = sum(sb[SB_SLOT_TABLE + i * SLOT_ENTRY_SIZE] for i in range(SLOT_COUNT))
    verdict = f"ok: {records(used)}"
    if sb[SB_INTENT_KIND] == INTENT_WRITE:
        verdict += ", an interrupted write will be discarded"
    print(verdict)
    return 0


if __name__ == "__main__":
    sys.exit(main())
