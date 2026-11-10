#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual CIO register helpers against a fault-injected MMIO model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MUTATIONS = {
    'skip-partitions': ('i < s->partitions; i++', 'i < 0; i++'),
    'truncate-streams': ('sid < s->streams; sid++', 'sid < 28; sid++'),
    'wrong-error-mask': ('#define DART_POWER_ERROR_MASK 0x4060', '#define DART_POWER_ERROR_MASK 0x104'),
    'truncate-faults': ('i < DIV_ROUND_UP(s->streams, 32); i++', 'i < 1; i++'),
    'skip-restore-flush': ('ret = dart_power_flush(regs, false) ?:', 'ret = 0 ?:'),
    'ignore-lock': ('if (locked)\n\t\treturn -EUCLEAN;', 'if (false)\n\t\treturn -EUCLEAN;'),
    'wrong-outstanding': ('regs + 0x230 + 4 * i', 'regs + 0x6000 + 4 * i'),
    'skip-params': ('if (readl(regs + 4 * i) != s->params[i])', 'if (false)'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    paths = ['drivers/iommu/apple-dart-power.h', 'drivers/iommu/apple-dart-cio-power.h']
    sources = {p: (ROOT / p).read_text() for p in paths}
    source = '\n'.join(sources.values())
    fixture = Path(__file__).with_suffix('.c').read_text()
    variants = {'real': source}
    for name, (old, new) in MUTATIONS.items():
        assert old in source, name
        variants[name] = source.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='dart-cio-power-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        results = {}
        for name, prod in variants.items():
            unit, binary = out / (name + '.c'), out / name
            unit.write_text(fixture.replace('/* ACTUAL_HEADERS */', prod))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-type-limits', '-g',
                            '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            results[name] = {'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
            assert (run.returncode == 0) == (name == 'real'), (name, results[name])
            if name == 'real':
                print(run.stdout, end='')
        (out / 'receipt.json').write_text(json.dumps({
            'sha256': {p: hashlib.sha256(s.encode()).hexdigest() for p, s in sources.items()},
            'results': results}, indent=2) + '\n')
        print(f'PASS {len(MUTATIONS)} negative controls')


if __name__ == '__main__':
    main()
