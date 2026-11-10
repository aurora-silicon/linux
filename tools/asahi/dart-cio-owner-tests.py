#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual owner transaction, writer gate and register helpers together."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    found = re.search(r'^(?:static )?(?:int|void|bool) ' + name + r'\([^;]+?\{.*?\n\}',
                      source, re.M | re.S)
    assert found, name
    return found.group()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    paths = ['drivers/iommu/apple-dart-power.h', 'drivers/iommu/apple-dart-cio-power.h',
             'drivers/iommu/apple-dart-cio.c', 'drivers/iommu/apple-dart-sleep.c',
             'drivers/iommu/apple-dart-cio-owner.h']
    sources = {p: (ROOT / p).read_text() for p in paths}
    hw = '\n'.join(sources[p] for p in paths[:2])
    owner = sources[paths[2]]
    struct = re.search(r'^struct apple_dart_cio \{.*?\n\};', sources[paths[4]],
                       re.M | re.S).group()
    names = ['apple_dart_cio_prepare', 'apple_dart_cio_gapf', 'apple_dart_cio_reopen',
             'apple_dart_cio_off', 'apple_dart_cio_on', 'apple_dart_cio_finish']
    methods = '\n'.join(function(owner, n) for n in names)
    gate = '\n'.join(function(sources[paths[3]], n) for n in [
        'apple_dart_sleep_writer_get', 'apple_dart_sleep_writer_put', 'apple_dart_sleep_close'])
    fixture = Path(__file__).with_name('dart-cio-power-tests.c').read_text().split('int main(void)')[0]
    fixture = fixture.replace('/* ACTUAL_HEADERS */', hw)
    fixture += Path(__file__).with_suffix('.c').read_text().replace('/* ACTUAL_OWNER */', struct + '\n' + gate + methods)
    mutations = {
        'allocator-is-not-hardware-size': (
            'streams = FIELD_GET(GENMASK(8, 0), params3);', 'streams = dart->num_streams;'),
        'skip-writer-close': ('ret = apple_dart_sleep_close(dart);', 'ret = 0;'),
        'ignore-gapf-policy': ('if (!ret && !(value & BIT(1)))', 'if (false)'),
        'forget-partial-off': ('s->changed = true;', 's->changed = false;'),
        'open-on-failure': ('\t\treturn ret;\n\t}\n\tdma_wmb();',
                            '\t\tapple_dart_cio_reopen(s);\n\t\treturn ret;\n\t}\n\tdma_wmb();'),
        'forget-stale-flush': ('if (stale) {', 'if (false) {'),
        'open-with-writers': ('if (!s->closed_writers) {', 'if (true) {'),
        'owner-only-gate': ('(READ_ONCE(cio->owner) || READ_ONCE(dart->sleep_closed))',
                            'READ_ONCE(cio->owner)'),
        'open-after-failed-flush': ('if (ret)\n\t\t\t\treturn ret;\n\t\t\tcontinue;',
                                    '(void)ret;\n\t\t\tcontinue;'),
        'forget-dead': ('\t\ts->dead = true;\n', ''),
        'reopen-unrestored': ('\tif (s->changed && !s->restored) {', '\tif (false) {'),
        'overwrite-gapf-neighbors': ('APPLE_PS_PROT_SLICE(s->slice), BIT(4),',
                                    'APPLE_PS_PROT_SLICE(s->slice), ~0U,'),
    }
    variants = {'real': fixture}
    for name, (old, new) in mutations.items():
        assert old in fixture, name
        variants[name] = fixture.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='dart-cio-owner-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        results = {}
        for name, text in variants.items():
            unit, binary = out / (name + '.c'), out / name
            unit.write_text(text)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-unused-parameter', '-g',
                            '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            results[name] = {'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
            assert (run.returncode == 0) == (name == 'real'), (name, results[name])
            if name == 'real': print(run.stdout, end='')
        (out / 'receipt.json').write_text(json.dumps({
            'sha256': {p: hashlib.sha256(s.encode()).hexdigest() for p, s in sources.items()},
            'results': results}, indent=2) + '\n')
        print(f'PASS {len(mutations)} negative controls')


if __name__ == '__main__':
    main()
