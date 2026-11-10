#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check public Apple device coverage manifests against compiled device trees.

No ADT input is accepted. Install python3-libfdt (or pylibfdt) to read DTBs.
--report permits MISSING entries for inventory, but still rejects broken paths,
malformed manifests, missing DTBs and empty selections.
With --baseline-manifest-dir and --baseline-dtb-dir, only coverage regressions fail.
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


def inventory(manifest, dtb, board):
    paths = node_paths(dtb, board)
    entries = list(manifest_entries(manifest))
    counts = Counter(entry[3] for entry in entries)
    broken = {entry[4] for entry in entries if entry[3] == "mapped" and entry[4] not in paths}
    return entries, counts, broken


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-dir", type=Path, default=Path(__file__).resolve().parents[2] /
                        "arch/arm64/boot/dts/apple/adt-coverage")
    parser.add_argument("--dtb-dir", type=Path, required=True)
    parser.add_argument("--board", action="append", default=[])
    parser.add_argument("--report", action="store_true", help="permit MISSING, but reject broken paths")
    parser.add_argument("--baseline-manifest-dir", type=Path,
                        help="compare with these base manifests; an empty directory bootstraps from head")
    parser.add_argument("--baseline-dtb-dir", type=Path, help="DTBs built at the base revision")
    args = parser.parse_args(argv)
    if bool(args.baseline_manifest_dir) != bool(args.baseline_dtb_dir):
        parser.error("both baseline directories are required")
    if args.report and args.baseline_manifest_dir:
        parser.error("--report and baseline comparison are separate modes")
    try:
        import libfdt
    except ImportError:
        parser.exit(2, "apple-adt-coverage: install python3-libfdt (pylibfdt)\n")

    for directory in (args.manifest_dir, args.baseline_manifest_dir):
        if directory is not None and not directory.is_dir():
            parser.error(f"manifest directory does not exist: {directory}")

    head_boards = {p.stem for p in args.manifest_dir.glob("t*-*.txt")}
    base_boards = ({p.stem for p in args.baseline_manifest_dir.glob("t*-*.txt")}
                   if args.baseline_manifest_dir else set())
    boards = args.board or sorted(head_boards | base_boards)
    if not boards:
        parser.error("no board manifests selected")
    errors = []
    for board in sorted(set(boards)):
        if not re.fullmatch(r"t[0-9]+-[a-z][a-zA-Z0-9]*", board):
            errors.append(f"invalid board name: {board}")
            continue
        manifest = args.manifest_dir / (board + ".txt")
        try:
            entries, counts, broken = inventory(manifest, args.dtb_dir / (board + ".dtb"), board)
            print(f"{board}: MISSING={counts['missing']} mapped={counts['mapped']} "
                  f"waived={counts['waived']} broken={len(broken)} total={len(entries)}")
            if args.baseline_manifest_dir:
                baseline = args.baseline_manifest_dir / (board + ".txt")
                bootstrap = not baseline.is_file()
                if bootstrap:
                    # First introduction: measure the same inventory against base
                    # DTBs. Later runs always use the base's committed inventory.
                    baseline = manifest
                    print(f"{board}: bootstrapping baseline from head inventory")
                    if not (args.baseline_dtb_dir / (board + ".dtb")).is_file():
                        # The board itself is new: nothing at the base can regress,
                        # but its own inventory must still resolve in the head DTB.
                        print(f"{board}: new board, no base DTB")
                        for target in sorted(broken):
                            errors.append(f"{board}: new absent DT path: {target}")
                        continue
                old, before, old_broken = inventory(
                    baseline, args.baseline_dtb_dir / (board + ".dtb"), board)
                removed = {e[1] for e in old} - {e[1] for e in entries}
                if removed:
                    errors.append(f"{board}: removed inventory devices: " + ", ".join(sorted(removed)))
                print(f"{board}: base MISSING={before['missing']} broken={len(old_broken)}; "
                      f"head MISSING={counts['missing']} broken={len(broken)}")
                if counts['missing'] > before['missing']:
                    errors.append(f"{board}: MISSING increased {before['missing']} -> {counts['missing']}")
                for target in sorted(broken if bootstrap else broken - old_broken):
                    errors.append(f"{board}: new absent DT path: {target}")
            else:
                for target in sorted(broken):
                    errors.append(f"{board}: absent DT path: {target}")
                if not args.report:
                    for _, name, address, kind, _ in entries:
                        if kind == "missing":
                            errors.append(f"{board}: MISSING: {name} {address}")
        except (OSError, UnicodeError, ValueError, libfdt.FdtException) as error:
            errors.append(f"{board}: {error}")
    for error in errors:
        print(error, file=sys.stderr)
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())
