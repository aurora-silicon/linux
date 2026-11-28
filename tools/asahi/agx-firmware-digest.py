#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Report the firmware builds inside an Apple AGX firmware container."""
import argparse
import hashlib
import re
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
ENTROPY_MARKER = b"GKTS\x08\x00\x00\x00"
IMAGE_INFO_OFFSET = 0x4200
IMAGE_INFO_SIZE = 0x38
IMAGE_INFO_BRANCH = 0x14000011
IMAGE_INFO_MAGIC = 0x64697575
IMAGE_INFO_VERSION = 5


def format_uuid(raw):
    h = raw.hex()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"


def load_commands(blob, start):
    """The Mach-O segments as (name, vmsize, fileoff, filesize), and the LC_UUID."""
    magic, _ct, _cs, _ft, ncmds = struct.unpack_from("<5I", blob, start)
    if magic != MH_MAGIC_64:
        return None, None
    off, segments, uuid = start + 32, [], None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", blob, off)
        if cmdsize == 0:
            break
        if cmd == LC_SEGMENT_64:
            name = blob[off + 8:off + 24].rstrip(b"\0").decode("ascii", "replace")
            _va, vmsize, fileoff, filesize = struct.unpack_from("<4Q", blob, off + 24)
            segments.append((name, vmsize, fileoff, filesize))
        elif cmd == LC_UUID:
            uuid = format_uuid(blob[off + 8:off + 24])
        off += cmdsize
    return segments, uuid


def rkosftab(blob):
    """The RTKit OS firmware table entries as (tag, offset, size), or None."""
    i = blob.find(b"rkosftab")
    if i < 0:
        return None
    count = struct.unpack_from("<Q", blob, i + 8)[0]
    if not 0 < count < 64:
        return None
    entries, off = [], i + 16
    for _ in range(count):
        tag = blob[off:off + 4].decode("ascii", "replace")
        offset, size = struct.unpack_from("<IQ", blob, off + 4)
        entries.append((tag, offset, size))
        off += 16
    return entries


def image_info(text):
    """The header the firmware publishes at __TEXT+0x4200, or None."""
    header = text[IMAGE_INFO_OFFSET:IMAGE_INFO_OFFSET + IMAGE_INFO_SIZE]
    if len(header) < IMAGE_INFO_SIZE:
        return None
    word = lambda at: struct.unpack_from("<I", header, at)[0]
    if word(0) != IMAGE_INFO_BRANCH or word(4) != IMAGE_INFO_MAGIC:
        return None
    if word(8) != IMAGE_INFO_VERSION or word(0x34) != len(text):
        return None
    start = word(0x2C)
    return {
        "uuid": format_uuid(header[0x14:0x24]),
        "patchbay": (start, word(0x30)),
        "text_size": word(0x34),
    }


def markers(text):
    return [m.start() for m in re.finditer(re.escape(ENTROPY_MARKER), text)]


def digest(text, ranges):
    copy = bytearray(text)
    for start, length in ranges:
        if start + length > len(copy):
            raise SystemExit(f"range {start:#x}:{length} runs past __TEXT")
        copy[start:start + length] = b"\0" * length
    return hashlib.sha256(copy).hexdigest()


def parse_range(text):
    start, _, length = text.partition(":")
    if not length:
        raise argparse.ArgumentTypeError("expected OFFSET:LENGTH")
    return int(start, 0), int(length, 0)


def report_entry(blob, start, tag, extra_ranges):
    segments, uuid = load_commands(blob, start)
    if segments is None:
        return
    print(f"=== entry {tag} at {start:#x} ===")
    print(f"  LC_UUID          {uuid}")
    text = None
    for name, vmsize, fileoff, filesize in segments:
        print(f"  {name:<16} vmsize {vmsize:#x} fileoff {fileoff:#x} filesize {filesize:#x}")
        if name == "__TEXT":
            text = blob[start + fileoff:start + fileoff + filesize]
    if text is None:
        print("  no __TEXT segment\n")
        return

    info = image_info(text)
    if info is None:
        print(f"  image info       absent at {IMAGE_INFO_OFFSET:#x}")
    else:
        patchbay_start, patchbay_size = info["patchbay"]
        print(f"  image info uuid  {info['uuid']}")
        print(f"  patchbay         {patchbay_start:#x}..{patchbay_start + patchbay_size:#x}"
              f" ({patchbay_size} bytes)")

    hits = markers(text)
    print(f"  entropy markers  {len(hits)} at {[hex(h) for h in hits]}")
    print(f"  sha256 raw       {digest(text, [])}")
    if len(hits) == 1:
        at = hits[0] + len(ENTROPY_MARKER)
        print(f"  entropy bytes    {text[at:at + 8].hex()}")
        print(f"  sha256 entropy   {digest(text, [(at, 8)])}")
    if info is not None:
        print(f"  sha256 patchbay  {digest(text, [info['patchbay']])}")
    if extra_ranges:
        print(f"  sha256 zeroed    {digest(text, extra_ranges)}"
              f"  ({len(extra_ranges)} range(s))")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="an armfw_g1*.im4p or agx*.im4p file, from G13 to G17")
    parser.add_argument("--zero", type=parse_range, action="append", default=[],
                        metavar="OFFSET:LENGTH",
                        help="zero this __TEXT range before hashing; repeat to"
                             " reproduce a boot_filled list")
    args = parser.parse_args()

    blob = open(args.image, "rb").read()
    print(f"file             {args.image}")
    print(f"size             {len(blob)}")
    print(f"sha256           {hashlib.sha256(blob).hexdigest()}")

    entries = rkosftab(blob)
    starts = [m.start() for m in re.finditer(b"\xcf\xfa\xed\xfe", blob)]
    print(f"rkosftab entries {len(entries) if entries else 'none'}")
    print(f"mach-o images    {len(starts)}\n")
    if entries:
        for tag, offset, size in entries:
            print(f"  tag {tag!r}  offset {offset:#x}  size {size:#x}")
        print()

    for i, start in enumerate(starts):
        tag = repr(entries[i][0]) if entries and i < len(entries) else f"image{i}"
        report_entry(blob, start, tag, args.zero)


if __name__ == "__main__":
    sys.exit(main())
