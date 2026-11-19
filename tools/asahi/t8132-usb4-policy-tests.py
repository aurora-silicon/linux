#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that the compiled legacy policy preserves USB4's PMGR providers.

Usage: t8132-usb4-policy-tests.py --dtc DTC --output DIR
Builds the T8132 platform data and the J713 and J715 trees from this checkout
with the given dtc, as the kernel build does. No hardware access.
"""

import argparse
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BOARDS = ('t8132-j713', 't8132-j715')


class Tree:
    """A flattened tree's nodes in depth-first order, with their paths and properties."""

    def __init__(self, blob):
        magic, _, off_struct, off_strings = struct.unpack_from('>4I', blob)
        assert magic == 0xd00dfeed, 'Not a flattened device tree'
        self.paths, self.props, self.children = [], [], []
        stack, pos = [], off_struct
        while True:
            token, = struct.unpack_from('>I', blob, pos)
            pos += 4
            if token == 1:  # FDT_BEGIN_NODE
                end = blob.index(b'\0', pos)
                name = blob[pos:end].decode()
                pos = (end + 4) & ~3
                node = len(self.paths)
                if stack:
                    self.children[stack[-1]].append(node)
                    self.paths.append(self.paths[stack[-1]].rstrip('/') + '/' + name)
                else:
                    self.paths.append('/')
                self.props.append({})
                self.children.append([])
                stack.append(node)
            elif token == 2:  # FDT_END_NODE
                stack.pop()
            elif token == 3:  # FDT_PROP
                length, nameoff = struct.unpack_from('>II', blob, pos)
                pos += 8
                start = off_strings + nameoff
                name = blob[start:blob.index(b'\0', start)].decode()
                self.props[stack[-1]][name] = blob[pos:pos + length]
                pos = (pos + length + 3) & ~3
            elif token == 9:  # FDT_END
                assert not stack, 'Unterminated node'
                return
            else:
                assert token == 4, f'Bad structure token {token:#x}'  # FDT_NOP

    def get_path(self, node):
        return self.paths[node]


def nodes(tree, parent=0):
    yield parent
    for child in tree.children[parent]:
        yield from nodes(tree, child)


def prop(tree, node, name):
    return tree.props[node].get(name, b'')


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


def build(dtc, source, dtb):
    """Preprocess and compile one tree, as scripts/Makefile.dtbs does."""
    text = subprocess.check_output([
        'cpp', '-nostdinc', '-undef', '-D__DTS__',
        '-I', str(ROOT / 'scripts/dtc/include-prefixes'),
        '-x', 'assembler-with-cpp', str(source),
    ])
    subprocess.run([dtc, '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb)], input=text, check=True)
    return Tree(dtb.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--dtc', required=True, help="this kernel's built scripts/dtc/dtc")
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    policy = build(args.dtc, ROOT / 'drivers/soc/apple/t8132-platform-data.dts',
                   args.output / 't8132-platform-data.dtb')
    for board in BOARDS:
        tree = build(args.dtc, ROOT / f'arch/arm64/boot/dts/apple/{board}.dts',
                     args.output / f'{board}.dtb')
        print(f'{board}.dtb: {check(policy, tree)} USB4 PMGR providers available')


if __name__ == '__main__':
    main()
