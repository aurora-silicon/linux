#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that the compiled legacy policy preserves USB4's PMGR providers.

Usage: t8132-usb4-policy-tests.py POLICY.dtb BOARD.dtb [BOARD.dtb ...]
Requires the Python libfdt module. No hardware access.
"""

from pathlib import Path
import struct
import sys

import libfdt


def nodes(tree, parent=0):
    yield parent
    child = tree.first_subnode(parent, quiet=(libfdt.NOTFOUND,))
    while child >= 0:
        yield from nodes(tree, child)
        child = tree.next_subnode(child, quiet=(libfdt.NOTFOUND,))


def prop(tree, node, name):
    value = tree.getprop(node, name, quiet=(libfdt.NOTFOUND,))
    return b'' if isinstance(value, int) else bytes(value)


def words(value):
    assert len(value) % 4 == 0, 'Malformed cell list'
    return struct.unpack('>' + 'I' * (len(value) // 4), value)


def compatible(tree, node, name):
    return name.encode() in prop(tree, node, 'compatible').split(b'\0')


def enabled(tree, node):
    return prop(tree, node, 'status') in (b'', b'ok\0', b'okay\0')


def check(policy, board):
    paths = {policy.get_path(n): n for n in nodes(policy)}
    phandles = {words(prop(board, n, 'phandle'))[0]: n for n in nodes(board)
                if prop(board, n, 'phandle')}
    pending = [n for n in nodes(board) if enabled(board, n) and
               any(compatible(board, n, c) for c in
                   ('apple,t8132-usb4-acio', 'apple,t8132-pciec'))]
    assert len(pending) == 4, 'Expected two routers and two tunnel hosts'
    seen, checked, scoped, failures = set(), set(), set(), []
    while pending:
        node = pending.pop()
        if node in seen:
            continue
        seen.add(node)
        for name, cells in (('resets', '#reset-cells'),
                            ('power-domains', '#power-domain-cells')):
            refs = words(prop(board, node, name))
            pos = 0
            while pos < len(refs):
                provider = phandles[refs[pos]]
                count = words(prop(board, provider, cells))
                assert len(count) == 1, 'Missing provider cell count'
                pos += 1 + count[0]
                assert pos <= len(refs), 'Truncated provider reference'
                if not compatible(board, provider, 'apple,t8132-pmgr-pwrstate'):
                    continue
                path = board.get_path(provider)
                checked.add(path)
                # On the legacy Air tree, disabled policy entries override
                # explicit board enables before the provider can probe.
                policy_node = paths.get(path)
                if (not enabled(board, provider) or policy_node is None or
                        not enabled(policy, policy_node)):
                    failures.append(path)
                # An entry the SoC tree leaves unset must not enable it on other boards.
                machine = prop(policy, policy_node, 'apple,policy-machine') \
                    if policy_node is not None else b''
                if machine and machine != b'apple,j713\0':
                    failures.append(path)
                if machine:
                    scoped.add(path)
                pending.append(provider)
    assert checked, 'No PMGR dependencies checked'
    assert not failures, 'USB4 providers disabled by policy: ' + ', '.join(sorted(set(failures)))
    # The ACIO states the board enables itself are scoped to it in the policy.
    assert len(scoped) == 8, 'J713-scoped ACIO policy entries: %d' % len(scoped)
    return len(checked)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    policy = libfdt.Fdt(Path(sys.argv[1]).read_bytes())
    for name in sys.argv[2:]:
        count = check(policy, libfdt.Fdt(Path(name).read_bytes()))
        print(f'{Path(name).name}: {count} USB4 PMGR providers available')


if __name__ == '__main__':
    main()
