#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the CPU order required by m1n1's sequential MPIDR validation."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BOARDS = {'t6040-j614s': 5, 't6040-j616s': 5, 't6040-j616s-tether': 5, 't6041-j616c': 6}


def check(dtb, board, cores):
    def get(*options):
        return subprocess.check_output(['fdtget', *options, str(dtb), '/cpus'], text=True)

    actual = []
    for node in get('-l').split():
        if not node.startswith('cpu@'):
            continue
        cells = subprocess.check_output(
            ['fdtget', '-t', 'x', str(dtb), '/cpus/' + node, 'reg'], text=True).split()
        actual.append((int(cells[0], 16) << 32) | int(cells[1], 16))
    expected = list(range(4)) + [base + core for base in [0x10100, 0x10200]
                                for core in range(cores)]
    assert actual == expected, f'{board} CPU MPIDR order: {actual!r}; expected {expected!r}'
    print(f'PASS: {board} CPU order ({len(actual)} cores)')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dtc', required=True, help="this kernel's built scripts/dtc/dtc")
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for board, cores in BOARDS.items():
        source = subprocess.check_output([
            'cpp', '-nostdinc', '-undef', '-D__DTS__',
            '-I', str(ROOT / 'scripts/dtc/include-prefixes'),
            '-x', 'assembler-with-cpp', str(ROOT / f'arch/arm64/boot/dts/apple/{board}.dts'),
        ])
        dtb = args.output / f'{board}.dtb'
        subprocess.run([args.dtc, '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb)],
                       input=source, check=True)
        check(dtb, board, cores)


if __name__ == '__main__':
    main()
