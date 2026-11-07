#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check public Apple device coverage manifests against compiled device trees.

No ADT input is accepted. Install python3-libfdt (or pylibfdt) to read DTBs.
--report permits MISSING entries for inventory, but still rejects broken paths,
malformed manifests, missing DTBs and empty selections.
"""

import argparse
from collections import Counter
from pathlib import Path
import re
import sys


def node_paths(filename, board):
    """Read every node, including disabled nodes, using the DT compiler library."""
    import libfdt

    fdt = libfdt.Fdt(filename.read_bytes())
    compatible = bytes(fdt.getprop(0, "compatible")).rstrip(b"\0").decode("ascii").split("\0")
    if not {"apple," + part for part in board.split("-", 1)} <= set(compatible):
        raise ValueError(f"{filename}: root compatible does not match {board}")
    paths = set()

    def walk(offset, path):
        if path in paths:
            raise ValueError(f"duplicate DT node: {path}")
        paths.add(path)
        child = fdt.first_subnode(offset, [libfdt.NOTFOUND])
        while child >= 0:
            walk(child, path.rstrip("/") + "/" + fdt.get_name(child))
            child = fdt.next_subnode(child, [libfdt.NOTFOUND])

    walk(0, "/")
    return paths


def manifest_entries(filename):
    seen = set()
    for number, text in enumerate(filename.read_text(encoding="ascii").splitlines(), 1):
        line = text.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"(/[^\s]+) ([^\s]+) -> (.+)", line)
        if not match:
            raise ValueError(f"{filename}:{number}: invalid entry")
        name, address, target = match.groups()
        if not re.fullmatch(r"-|0x[0-9a-f]+|/[A-Za-z0-9_,.:+@/~\-]+@0x[0-9a-f]+", address):
            raise ValueError(f"{filename}:{number}: expected physical or bus address, or -")
        if (name, address) in seen:
            raise ValueError(f"{filename}:{number}: duplicate device {name} {address}")
        seen.add((name, address))
        if target.startswith("/"):
            if (re.search(r"\s", target) or "//" in target or
                    any(p in (".", "..") for p in target.split("/")) or
                    (target != "/" and target.endswith("/"))):
                raise ValueError(f"{filename}:{number}: invalid DT path")
            kind = "mapped"
        elif target == "MISSING":
            kind = "missing"
        elif target.startswith("WAIVED: ") and target[8:].strip():
            kind = "waived"
        else:
            raise ValueError(f"{filename}:{number}: expected DT path, MISSING or reasoned WAIVED")
        yield number, name, address, kind, target
    if not seen:
        raise ValueError(f"{filename}: empty manifest")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-dir", type=Path, default=Path(__file__).resolve().parents[2] /
                        "arch/arm64/boot/dts/apple/adt-coverage")
    parser.add_argument("--dtb-dir", type=Path, required=True,
                        help="directory containing <soc>-<board>.dtb (usually out/.../apple)")
    parser.add_argument("--board", action="append", default=[],
                        help="check this manifest stem; repeat for multiple boards")
    parser.add_argument("--report", action="store_true", help="inventory MISSING without failing on it")
    args = parser.parse_args(argv)
    try:
        import libfdt
    except ImportError:
        parser.exit(2, "apple-adt-coverage: install python3-libfdt (pylibfdt)\n")

    boards = args.board or sorted(p.stem for p in args.manifest_dir.glob("t*-*.txt"))
    if not boards:
        parser.error("no board manifests selected")
    errors = 0
    for board in sorted(set(boards)):
        if not re.fullmatch(r"t[0-9]+-[a-z][a-zA-Z0-9]*", board):
            print(f"invalid board name: {board}", file=sys.stderr)
            errors += 1
            continue
        manifest = args.manifest_dir / (board + ".txt")
        try:
            paths = node_paths(args.dtb_dir / (board + ".dtb"), board)
            entries = list(manifest_entries(manifest))
            counts = Counter(entry[3] for entry in entries)
            for number, name, address, kind, target in entries:
                if kind == "mapped" and target not in paths:
                    print(f"{manifest}:{number}: absent DT path: {target}", file=sys.stderr)
                    errors += 1
                elif kind == "missing" and not args.report:
                    print(f"{manifest}:{number}: MISSING: {name} {address}", file=sys.stderr)
                    errors += 1
            print(f"{board}: MISSING={counts['missing']} mapped={counts['mapped']} "
                  f"waived={counts['waived']} total={len(entries)}")
        except (OSError, UnicodeError, ValueError, libfdt.FdtException) as error:
            print(f"{board}: {error}", file=sys.stderr)
            errors += 1
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())
