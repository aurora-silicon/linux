#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Pull one member out of a remote ZIP, such as an Apple OS firmware asset."""
import argparse
import os
import struct
import sys
import urllib.request
import zlib

USER_AGENT = "aurora-asset-range-fetch/1"
EOCD = b"PK\x05\x06"
EOCD64_LOCATOR = b"PK\x06\x07"
EOCD64 = b"PK\x06\x06"
CENTRAL_HEADER = b"PK\x01\x02"
LOCAL_HEADER = b"PK\x03\x04"
STORED, DEFLATED = 0, 8


def fetch(url, start, length):
    request = urllib.request.Request(url, headers={
        "User-Agent": USER_AGENT,
        "Range": f"bytes={start}-{start + length - 1}",
    })
    with urllib.request.urlopen(request, timeout=120) as response:
        if response.status != 206:
            raise SystemExit("the server ignored the range request")
        return response.read()


def archive_size(url):
    request = urllib.request.Request(url, method="HEAD",
                                     headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers["Content-Length"])


def central_directory(url, total):
    tail_length = min(1 << 16, total)
    tail = fetch(url, total - tail_length, tail_length)
    at = tail.rfind(EOCD)
    if at < 0:
        raise SystemExit("no end-of-central-directory record")
    size, offset = struct.unpack_from("<II", tail, at + 12)
    locator = tail.rfind(EOCD64_LOCATOR)
    if locator >= 0:
        record = fetch(url, struct.unpack_from("<Q", tail, locator + 8)[0], 56)
        if record[:4] == EOCD64:
            size, offset = struct.unpack_from("<QQ", record, 40)
    return fetch(url, offset, size)


def zip64_extra(extra, uncompressed, compressed, header):
    """The ZIP64 extended information, which overrides the 0xffffffff placeholders."""
    at = 0
    while at < len(extra) - 3:
        tag, size = struct.unpack_from("<2H", extra, at)
        if tag == 0x0001:
            values, cursor = [], at + 4
            for placeholder in (uncompressed, compressed, header):
                if placeholder == 0xFFFFFFFF and cursor + 8 <= at + 4 + size:
                    values.append(struct.unpack_from("<Q", extra, cursor)[0])
                    cursor += 8
                else:
                    values.append(placeholder)
            uncompressed, compressed, header = values
        at += 4 + size
    return uncompressed, compressed, header


def members(central):
    at = 0
    while at < len(central) - 4 and central[at:at + 4] == CENTRAL_HEADER:
        (method,) = struct.unpack_from("<H", central, at + 10)
        compressed, uncompressed = struct.unpack_from("<II", central, at + 20)
        name_length, extra_length, comment_length = struct.unpack_from("<3H", central, at + 28)
        (header,) = struct.unpack_from("<I", central, at + 42)
        name = central[at + 46:at + 46 + name_length].decode("utf-8", "replace")
        extra = central[at + 46 + name_length:at + 46 + name_length + extra_length]
        uncompressed, compressed, header = zip64_extra(
            extra, uncompressed, compressed, header)
        yield name, method, compressed, uncompressed, header
        at += 46 + name_length + extra_length + comment_length


def write_member(url, name, method, compressed, header, outdir):
    head = fetch(url, header, 30)
    if head[:4] != LOCAL_HEADER:
        raise SystemExit(f"{name}: bad local file header")
    name_length, extra_length = struct.unpack_from("<2H", head, 26)
    data = fetch(url, header + 30 + name_length + extra_length, compressed)
    if method == STORED:
        blob = data
    elif method == DEFLATED:
        blob = zlib.decompressobj(-zlib.MAX_WBITS).decompress(data)
    else:
        raise SystemExit(f"{name}: unsupported compression method {method}")
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, os.path.basename(name))
    with open(path, "wb") as handle:
        handle.write(blob)
    print(f"wrote {path} ({len(blob)} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="an archive on a server that accepts range requests")
    parser.add_argument("pattern", help="case-insensitive substring of the member name")
    parser.add_argument("outdir", nargs="?", default=".", help="where to write the members")
    parser.add_argument("--list", action="store_true", help="list the matches and stop")
    args = parser.parse_args()

    total = archive_size(args.url)
    central = central_directory(args.url, total)
    print(f"archive size {total}", file=sys.stderr)

    count, matches = 0, []
    for member in members(central):
        count += 1
        if args.pattern.lower() in member[0].lower():
            matches.append(member)
    print(f"members {count}, matching {args.pattern!r} {len(matches)}", file=sys.stderr)
    for name, method, compressed, uncompressed, _header in matches:
        print(f"  {name}  method {method} compressed {compressed} size {uncompressed}")
    if args.list or not matches:
        return 0
    for name, method, compressed, _uncompressed, header in matches:
        write_member(args.url, name, method, compressed, header, args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
