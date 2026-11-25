#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Warn when a change gives one board a property its sibling boards lack.

Boards of one SoC (t8112-j413, t8112-j415, t8112-j473, t8112-j493) share most of
their device tree through the SoC's .dtsi files. A fix written for one Mac and put
in that Mac's .dts leaves its siblings without it, although they have the same
block. For every SoC with two or more boards, this looks at the nodes all of its
boards have and lists each property that only some of them carry ("drift").

Much drift is right (a desktop's HDMI port, a laptop's lid), so the existing drift
is the baseline: with --baseline-dtb-dir only drift the change adds is reported,
as a warning that names the boards with and without the property. Drift the change
removes is counted as resolved. Only board trees (<soc>-<board>.dtb) are compared;
variants such as t8112-j413-fw27 are left out. --strict makes new drift fail.

Install python3-libfdt (or pylibfdt) to read DTBs.
"""

import argparse
from pathlib import Path
import re
import sys

try:
    import libfdt
except ImportError:
    sys.exit("apple-sibling-drift: needs python3-libfdt (pylibfdt)")

BOARD = re.compile(r"^(t\d{4})-(j\d+[a-z]*)\.dtb$")
SKIP_NODES = ("/aliases", "/chosen", "/memory", "/__symbols__", "/__fixups__",
              "/__local_fixups__")
SKIP_PROPS = {"status", "phandle", "linux,phandle"}


def node_props(path):
    fdt = libfdt.Fdt(path.read_bytes())
    nodes = {}

    def walk(offset, name):
        props = set()
        prop = fdt.first_property_offset(offset, libfdt.QUIET_NOTFOUND)
        while prop >= 0:
            props.add(fdt.get_property_by_offset(prop).name)
            prop = fdt.next_property_offset(prop, libfdt.QUIET_NOTFOUND)
        nodes[name] = props
        child = fdt.first_subnode(offset, libfdt.QUIET_NOTFOUND)
        while child >= 0:
            walk(child, name.rstrip("/") + "/" + fdt.get_name(child))
            child = fdt.next_subnode(child, libfdt.QUIET_NOTFOUND)

    walk(0, "/")
    return nodes


def drift(directory):
    socs = {}
    for path in sorted(Path(directory).glob("*.dtb")):
        match = BOARD.match(path.name)
        if match:
            socs.setdefault(match.group(1), {})[match.group(2)] = node_props(path)
    found = {}
    for soc, boards in socs.items():
        if len(boards) < 2:
            continue
        common = set.intersection(*(set(nodes) for nodes in boards.values()))
        for node in common:
            if node.startswith(SKIP_NODES):
                continue
            for prop in set.union(*(boards[b][node] for b in boards)) - SKIP_PROPS:
                have = frozenset(b for b in boards if prop in boards[b][node])
                if len(have) < len(boards):
                    found[(soc, node, prop)] = (have, frozenset(boards))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dtb-dir", required=True)
    parser.add_argument("--baseline-dtb-dir")
    parser.add_argument("--strict", action="store_true", help="fail on new drift")
    parser.add_argument("--report", action="store_true", help="list all drift")
    args = parser.parse_args()

    head = drift(args.dtb_dir)
    base = drift(args.baseline_dtb_dir) if args.baseline_dtb_dir else {}
    new = []
    for key, (have, boards) in sorted(head.items()):
        old = base.get(key)
        # New when the baseline had no drift here, or fewer boards carried it then.
        if args.report or (args.baseline_dtb_dir and (old is None or not have <= old[0])):
            new.append((key, have, boards))
    for (soc, node, prop), have, boards in new:
        lack = sorted(boards - have)
        print(f"::warning title=Sibling drift::{soc} {node} {prop}: only on "
              f"{', '.join(sorted(have))}; {', '.join(lack)} have the node without it. "
              f"Same block on every board: put it in the shared .dtsi; board-specific: "
              f"nothing to do.")
    resolved = sum(1 for key in base if key not in head)
    print(f"apple-sibling-drift: drift {len(head)}, new {len(new) if not args.report else '-'}"
          f", resolved {resolved}", file=sys.stderr)
    return 1 if args.strict and new and not args.report else 0


if __name__ == "__main__":
    sys.exit(main())
