#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compare production Rust firmware encoders against frozen independent C bytes."""
from pathlib import Path
import json
import subprocess
import time
from common import HERE, frozen_bindings, kernel_root, output_dir, rustc, sha, unpack_fixtures


def main():
    start = time.monotonic()
    root, out = kernel_root(), output_dir()
    golden = out / 'golden'
    manifest = unpack_fixtures(golden)
    (out / 'public-uapi.rs').write_bytes(frozen_bindings().read_bytes())
    backend = root / 'drivers/gpu/drm/asahi/g16'
    identity = (root / 'drivers/gpu/drm/asahi/fwid.rs').read_text()
    first = identity.index('pub(crate) struct FirmwareBuild {')
    build_type = identity[first:identity.index('\n}', first) + 2]
    logs = []
    for name in ['initdata', 'compute', 'render', 'channels', 'initdata-bounds']:
        source = (HERE / f'check-{name}.rs').read_text()
        source = source.replace('@KERNEL@', str(root)).replace('@OUTPUT@', str(out))
        source = source.replace('@FWID_BUILD@', build_type)
        test = out / f'check-{name}.rs'
        test.write_text(source)
        binary = out / f'check-{name}'
        argument = golden / ('channels-graph.bin' if name == 'channels' else 'initdata-bounds.bin') if name in ('channels', 'initdata-bounds') else golden
        for command in [rustc() + ['--edition=2021', '-Dwarnings', '-C', 'overflow-checks=yes', str(test), '-o', str(binary)], [str(binary), str(argument)]]:
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            logs.append(result.stdout)
            print(result.stdout, end='', flush=True)
            (out / 'golden-tests.log').write_text('\n'.join(logs))
            if result.returncode:
                return result.returncode
    report = {'passed': True, 'scope': 'Production Rust encoder output versus frozen independent C fixtures; CPU-only, no hardware acceptance.',
              'reference_kernel_commit': manifest['reference_kernel_commit'], 'fixture_manifest_sha256': sha(HERE / 'fixtures/manifest.json'),
              'seconds': round(time.monotonic()-start, 3), 'compared_encoder_bytes': 3736988,
              'source_sha256': {str(p.relative_to(root)): sha(p) for p in sorted(backend.rglob('*.rs'))}}
    (out / 'golden-tests.json').write_text(json.dumps(report, indent=2)+'\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
