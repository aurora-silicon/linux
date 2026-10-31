#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host controls for display-engine clock eligibility at the DCP adapter."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = {
    'include/linux/soc/apple/dp-tunnel.h': ['apple_dp_tunnel_t6030_pclk'],
    'drivers/gpu/drm/apple/dcp-fabric-core.c': ['dcp_fabric_tunnel_clock_blocked'],
    'drivers/gpu/drm/apple/dcp-fabric.c': [
        'dcp_fabric_snapshot_port', 'dcp_typec_tunnel_ctl'],
}
# Each must make the fixture fail.
MUTATIONS = {
    'clock-ignored': ('& BIT(dpin)))', '& 0))'),
    'snapshot-ignores-yield': ('port->dp_wanted && !port->typec_yielded;', 'port->dp_wanted;'),
}


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?(?:int|void|u8|struct mux_control \*)\s*' +
                      name + r'\([^;]*?\)\n\{', source, re.M | re.S)
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
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    bodies = [function((ROOT / path).read_text(), name)
              for path, names in FUNCTIONS.items() for name in names]
    header = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric-core.h').read_text()
    header = re.sub(r'^#include.*\n', '', header, flags=re.M)
    with tempfile.TemporaryDirectory(prefix='tunnel-routes-') as temporary:
        out = args.out or Path(temporary)
        out.mkdir(parents=True, exist_ok=True)
        code = (ROOT / 'tools/asahi/tunnel-route/fixture.c').read_text().replace(
            '/* CORE_HEADER */', header).replace(
            '/* PRODUCTION_FUNCTIONS */', '\n'.join(bodies))

        def build(name, text):
            source = out / (name + '.c')
            source.write_text(text)
            binary = out / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror',
                            str(source), '-o', str(binary)], check=True)
            return subprocess.run([str(binary)], capture_output=True, text=True)

        for name, (old, new) in MUTATIONS.items():
            assert code.count(old) == 1, 'stale mutation: ' + name
            mutant = build(name, code.replace(old, new))
            assert mutant.returncode, 'mutation survived: ' + name
            print('PASS negative control:', name)
        run = build('production', code)
        receipt = {
            'bodies': {name: hashlib.sha256(body.encode()).hexdigest()
                       for name, body in zip(
                           [n for names in FUNCTIONS.values() for n in names], bodies)},
            'translation_sha256': hashlib.sha256(code.encode()).hexdigest(),
            'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr,
        }
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(run.stdout, end='')
        if run.returncode:
            raise SystemExit(run.returncode)


if __name__ == '__main__':
    main()
