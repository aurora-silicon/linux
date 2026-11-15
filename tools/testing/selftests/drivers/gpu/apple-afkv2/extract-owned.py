#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production ownership checker with host-only synchronization mocks."""
import hashlib
from pathlib import Path
import sys


def main():
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    output = Path(sys.argv[2])
    first = "static int afk_v2_access_owned("
    last = "static void afk_v2_callback("
    if source.count(first) != 1 or source.count(last) != 1:
        raise SystemExit("FAIL: ownership function boundaries changed; review extractor")
    start = source.index(first)
    end = source.index(last, start)
    body = source[start:end]
    if "int afk_v2_copy_owned(" not in body:
        raise SystemExit("FAIL: production ownership wrapper missing")
    directory = Path(__file__).resolve().parent
    output.write_text((directory / "owned-prefix.c").read_text() + body +
                      (directory / "owned-suffix.c").read_text(), encoding="utf-8")
    print("Production ownership function SHA256:", hashlib.sha256(body.encode()).hexdigest())


if __name__ == "__main__":
    main()
