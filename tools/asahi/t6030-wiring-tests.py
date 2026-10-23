#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the shared DP wiring admission and DCP mux lookup on modeled DT nodes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = {
    'include/linux/soc/apple/dp-tunnel.h': [
        'apple_dp_tunnel_t6030_pclk', 'apple_dp_tunnel_wiring_dual',
        'apple_dp_tunnel_mux_matches', 'apple_dp_tunnel_t6030_legacy_mux',
        'apple_dp_tunnel_route_wired', 'apple_dp_tunnel_dual_stream'],
    'drivers/gpu/drm/apple/dcp-fabric-core.c': ['dcp_fabric_wiring'],
    'drivers/gpu/drm/apple/dcp-fabric.c': ['dcp_typec_route_endpoints', 'dcp_typec_route_wiring'],
}
CASES = ['tb-source0', 'tb-source2', 'dcp-source0', 'dcp-source2',
         'other-soc', 'odd-source', 'unknown-source', 'missing-source',
         'single-endpoint', 'wrong-provider', 'missing-primary', 'parse-failure',
         'primary-cell1', 'primary-two-args', 'named', 'named-other-soc',
         'named-wrong-cell', 'named-other-provider', 'partial-named',
         'usb4', 'usb4-other-soc', 'bad-usb4']

def function(source, name):
    match = re.search(r'^[^\n;{}]*\b' + name + r'\([^;]*?\)\n\{', source, re.M | re.S)
    if not match:
        raise ValueError(name)
    pos = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--old-ref')
    parser.add_argument('--old-classifier', choices=['tb', 'dcp'])
    args = parser.parse_args()
    bodies = {}
    for path, names in FUNCTIONS.items():
        source = (ROOT / path).read_text()
        for name in names:
            body = function(source, name)
            old_name = {'tb': 'apple_dp_tunnel_route_wired', 'dcp': 'dcp_typec_route_wiring'}.get(args.old_classifier)
            if args.old_ref and name == old_name:
                body = function(subprocess.check_output(['git', 'show', args.old_ref + ':' + path], cwd=ROOT, text=True), name)
            bodies[name] = body
    enum = re.search(r'enum dcp_fabric_wiring \{.*?\n\};',
                     (ROOT / 'drivers/gpu/drm/apple/dcp-fabric-core.h').read_text(), re.S).group()
    fixture = (ROOT / 'tools/asahi/t6030-wiring/fixture.c').read_text()
    code = fixture.replace('/* PRODUCTION_FUNCTIONS */', enum + '\n' + '\n'.join(bodies.values()))
    args.out.mkdir(parents=True, exist_ok=True)
    source, binary = args.out / 'production.c', args.out / 'production'
    source.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wno-unused-function', str(source), '-o', str(binary)], check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    results = []
    for case in CASES:
        run = subprocess.run([str(binary), case], capture_output=True, text=True, preexec_fn=no_core)
        results.append({'case': case, 'exit': run.returncode, 'stderr': run.stderr})
    receipt = {'production_bodies': {n: hashlib.sha256(b.encode()).hexdigest() for n, b in bodies.items()},
               'old_ref': args.old_ref, 'old_classifier': args.old_classifier,
               'hardware_access': False, 'results': results}
    (args.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    if any(r['exit'] for r in results):
        raise SystemExit(1)
if __name__ == '__main__':
    main()
