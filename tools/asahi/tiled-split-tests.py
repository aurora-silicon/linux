#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production DPIN1 dispatch and tiled-display split admission.

The production bodies are compiled with ASan/UBSan against a host fixture
that stubs only what lies below them. With --mutations, each mutant changes
the extracted bodies in memory (never the source) and must make at least one
case fail.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = {
    'drivers/gpu/drm/apple/dcp-fabric-session.h': ['dcp_fabric_binding_request'],
    'drivers/gpu/drm/apple/dcp-fabric-core.c': ['dcp_fabric_tunnel_slot'],
    'drivers/gpu/drm/apple/dcp-fabric.c': [
        'dcp_typec_port_untileable', 'dcp_tb_split_locked', 'dcp_tb_dispatch_request'],
}
CASES = ['t6030-dpin1-alone', 't6030-dpin1-alone-before-services',
         't6030-dpin1-with-external-owner', 't6030-dpin1-then-dpin0',
         't6030-dpin0-then-dpin1', 't6030-split-off', 't6030-inactive-dpin1',
         'internal-dpin1-alone-waits', 'internal-not-ready-waits',
         'mixed-routes-wait', 'no-routes-wait', 'internal-tile-hint-pending',
         'internal-tiled-join', 'internal-owner-without-port1',
         'split-retired-teardown-first', 'split-existing-release',
         'split-existing-active']
# (name, old, new): valid C that changes the admission.
MUTATIONS = [
    ('gate-removed', 'if (dcp_typec_port_untileable(port))',
     'if (false && dcp_typec_port_untileable(port))'),
    ('gate-before-split-handling',
     'lockdep_assert_held(&dcp_typec_fabric_lock);\n\t/* A retired provider',
     'lockdep_assert_held(&dcp_typec_fabric_lock);\n\tif (dcp_typec_port_untileable(port))\n'
     '\t\treturn -EOPNOTSUPP;\n\t/* A retired provider'),
    ('readiness-judged', 'if (!route->dcp->external)\n\t\t\treturn false;',
     'if (!route->dcp->external && route->dcp->dptxport[1].enabled)\n\t\t\treturn false;'),
    ('empty-untileable', 'return !list_empty(&port->routes);', 'return true;'),
    ('any-external-untileable', 'if (!route->dcp->external)\n\t\t\treturn false;',
     'if (route->dcp->external)\n\t\t\treturn true;'),
    ('gate-always', 'if (dcp_typec_port_untileable(port))',
     'if (true || dcp_typec_port_untileable(port))'),
]


def function(source, name):
    # A definition line: not a comment line that mentions the name.
    match = re.search(r'^(?![ \t]*(?:\*|/\*|//))[^\n;{}]*\b' + name + r'\([^;]*?\)\n\{',
                      source, re.M | re.S)
    if not match:
        raise ValueError(name)
    pos = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


def build(out, bodies, tag):
    fixture = (ROOT / 'tools/asahi/tiled-split/fixture.c').read_text()
    code = fixture.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(bodies.values()))
    source, binary = out / f'{tag}.c', out / tag
    source.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(source), '-o', str(binary)], check=True)
    return binary


def run(binary):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    results = []
    for case in CASES:
        r = subprocess.run([str(binary), case], capture_output=True, text=True,
                           preexec_fn=no_core)
        results.append({'case': case, 'exit': r.returncode, 'stderr': r.stderr.strip()[-300:]})
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--old-ref', help='take the dispatch and split bodies from this '
                        'commit instead (a negative control: expected to fail)')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    bodies = {}
    for path, names in FUNCTIONS.items():
        source = (ROOT / path).read_text()
        for name in names:
            bodies[name] = function(source, name)
            if args.old_ref and name in ('dcp_tb_split_locked', 'dcp_tb_dispatch_request'):
                old = subprocess.check_output(['git', 'show', f'{args.old_ref}:{path}'],
                                              cwd=ROOT, text=True)
                bodies[name] = function(old, name)
    results = run(build(args.out, bodies, 'production'))
    failed = [r for r in results if r['exit']]
    mutants = []
    if args.mutations:
        for name, old, new in MUTATIONS:
            text = '\n'.join(bodies.values())
            if text.count(old) != 1:
                raise SystemExit(f'mutation {name}: {text.count(old)} matches')
            mutated = {k: v.replace(old, new) for k, v in bodies.items()}
            mres = run(build(args.out, mutated, 'mutant-' + name))
            caught = [r['case'] for r in mres if r['exit']]
            mutants.append({'mutant': name, 'caught_by': caught, 'killed': bool(caught)})
    receipt = {'production_bodies': {n: hashlib.sha256(b.encode()).hexdigest() for n, b in bodies.items()},
               'old_ref': args.old_ref, 'hardware_access': False,
               'results': results, 'mutants': mutants}
    (args.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'{len(results) - len(failed)}/{len(results)} cases pass')
    for r in failed:
        print('FAIL', r['case'], r['stderr'])
    for m in mutants:
        print('KILLED' if m['killed'] else 'SURVIVED', m['mutant'], m['caught_by'][:4])
    if failed or any(not m['killed'] for m in mutants):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
