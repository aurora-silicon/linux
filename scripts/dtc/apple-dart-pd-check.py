#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that every Apple DART in the compiled device trees names a power domain.

A DART probes as a device of its own. Without its engine's power state it can be
probed, and read, while the block it sits in is off, which takes an SError. The
DART has no power gate of its own; the engine's covers it. So every node with an
"apple,*dart*" compatible must have power-domains, and an enabled DART must not
name a disabled power state (it would wait for it forever) unless its engine
names the same state, in which case the engine is not usable either.

Exempt: the DARTs of the SEP and of the AOP. Those coprocessors run from
before the handoff on states the AP never switches off.

Install python3-libfdt (or pylibfdt) to read DTBs. Labels are shown when the DTBs
were built with DTC_FLAGS=-@.

Without --baseline-dtb-dir every finding fails. With it, only findings that the
baseline DTBs do not have fail, so the existing ones can be burnt down while no
new DART lands without its domain.
"""

import argparse
from pathlib import Path
import re
import struct
import sys

DART = re.compile(r"apple,.*dart")
EXEMPT = (
    (re.compile(r"apple,sep$"), "SEP"),
    (re.compile(r"apple,(t\d+-)?aop$"), "AOP"),
)


def findings(filename):
    """Yield (path, label, compatible, status, problem) for one DTB."""
    import libfdt

    NF = (libfdt.NOTFOUND,)
    fdt = libfdt.Fdt(filename.read_bytes())
    nodes = {}

    def walk(offset, path):
        nodes[offset] = path
        child = fdt.first_subnode(offset, NF)
        while child >= 0:
            walk(child, path.rstrip("/") + "/" + fdt.get_name(child))
            child = fdt.next_subnode(child, NF)

    walk(0, "/")

    def prop(offset, name):
        value = fdt.getprop(offset, name, NF)
        return None if isinstance(value, int) else bytes(value)

    def cells(value):
        return struct.unpack(">%dI" % (len(value) // 4), value) if value else ()

    def strings(value):
        return value.rstrip(b"\0").decode().split("\0") if value else []

    def status(offset):
        return (strings(prop(offset, "status")) or ["okay"])[0]

    phandles = {fdt.get_phandle(o): o for o in nodes if fdt.get_phandle(o)}
    labels = {}
    symbols = fdt.path_offset("/__symbols__", NF)
    if symbols >= 0:
        p = fdt.first_property_offset(symbols, NF)
        while p >= 0:
            sym = fdt.get_property_by_offset(p)
            labels.setdefault(bytes(sym).rstrip(b"\0").decode(), sym.name)
            p = fdt.next_property_offset(p, NF)

    def iommu_cells(offset):
        value = cells(prop(offset, "#iommu-cells"))
        return value[0] if value else 1

    consumers = {}
    for offset in nodes:
        spec = cells(prop(offset, "iommus"))
        i = 0
        while i < len(spec) and spec[i] in phandles:
            consumers.setdefault(phandles[spec[i]], []).append(offset)
            i += 1 + iommu_cells(phandles[spec[i]])
        spec = cells(prop(offset, "iommu-map"))
        i = 0
        while i + 1 < len(spec) and spec[i + 1] in phandles:
            consumers.setdefault(phandles[spec[i + 1]], []).append(offset)
            i += 3 + iommu_cells(phandles[spec[i + 1]])

    for offset, path in nodes.items():
        compatible = strings(prop(offset, "compatible"))
        if not any(DART.match(c) for c in compatible):
            continue
        label = labels.get(path, "-")
        domains = cells(prop(offset, "power-domains"))
        if not domains:
            users = [c for o in consumers.get(offset, [])
                     for c in strings(prop(o, "compatible"))]
            why = [name for pattern, name in EXEMPT if any(pattern.match(c) for c in users)]
            problem = "exempt (%s)" % why[0] if why else "no power-domains"
            yield path, label, compatible[0], status(offset), problem
            continue
        if status(offset) != "okay":
            continue
        i = 0
        while i < len(domains):
            provider = phandles.get(domains[i])
            if provider is None:
                break
            if status(provider) != "okay":
                # Not a DART problem when its engine waits for the same state.
                users = [o for o in consumers.get(offset, []) if status(o) == "okay"]
                shared = users and all(domains[i] in cells(prop(o, "power-domains"))
                                       for o in users)
                yield (path, label, compatible[0], "okay",
                       "%spower domain %s is disabled" %
                       ("note: engine waits too, " if shared else "", nodes[provider]))
            n = cells(prop(provider, "#power-domain-cells"))
            i += 1 + (n[0] if n else 0)


def collect(directory):
    dtbs = sorted(Path(directory).glob("*.dtb"))
    if not dtbs:
        raise SystemExit("%s: no DTBs" % directory)
    result = {}
    for dtb in dtbs:
        for path, label, compatible, status, problem in findings(dtb):
            result[(dtb.stem, path, problem)] = (label, compatible, status)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dtb-dir", required=True,
                        help="directory with the compiled Apple DTBs")
    parser.add_argument("--baseline-dtb-dir",
                        help="DTBs of the base revision; only new findings fail")
    parser.add_argument("--report", action="store_true",
                        help="list every finding and exemption, never fail")
    args = parser.parse_args()

    head = collect(args.dtb_dir)
    base = collect(args.baseline_dtb_dir) if args.baseline_dtb_dir else {}
    failing = 0
    for key in sorted(head):
        board, path, problem = key
        label, compatible, status = head[key]
        exempt = problem.startswith(("exempt", "note:"))
        new = not exempt and (not args.baseline_dtb_dir or key not in base)
        if args.report or new:
            print("%s %s %s %s %s: %s%s" % (board, path, label, compatible, status, problem,
                                            " (new)" if new and args.baseline_dtb_dir else ""))
        failing += new
    counted = ("exempt", "note:")
    gone = [k for k in base if k not in head and not k[2].startswith(counted)]
    total = sum(1 for k in head if not k[2].startswith(counted))
    print("DARTs without a usable power domain: %d, new: %d, fixed: %d" %
          (total, failing if args.baseline_dtb_dir else total, len(gone)),
          file=sys.stderr)
    return 0 if args.report or not failing else 1


if __name__ == "__main__":
    sys.exit(main())
