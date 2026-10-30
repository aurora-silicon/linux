#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the CPU order required by m1n1's sequential MPIDR validation."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dtb')
    parser.add_argument('--soc', choices=['t6040', 't6041'], required=True)
    args = parser.parse_args()

    def get(*options):
        return subprocess.check_output(['fdtget', *options, args.dtb, '/cpus'], text=True)

    actual = []
    for node in get('-l').split():
        if not node.startswith('cpu@'):
            continue
        cells = subprocess.check_output(
            ['fdtget', '-t', 'x', args.dtb, '/cpus/' + node, 'reg'], text=True).split()
        actual.append((int(cells[0], 16) << 32) | int(cells[1], 16))
    cores = 5 if args.soc == 't6040' else 6
    expected = list(range(4)) + [base + core for base in [0x10100, 0x10200]
                                for core in range(cores)]
    assert actual == expected, f'CPU MPIDR order: {actual!r}; expected {expected!r}'
    print(f'PASS: {args.soc} CPU order ({len(actual)} cores)')


if __name__ == '__main__':
    main()
