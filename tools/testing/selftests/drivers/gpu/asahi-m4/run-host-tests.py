#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""CPU-only Asahi M4 regressions. Never opens a render node or submits GPU work."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from common import HERE, kernel_root, sha, verify_fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    if args.source:
        os.environ['ASAHI_M4_KERNEL'] = str(args.source.resolve())
    root = kernel_root()
    # Missing host prerequisites are kselftest skips, not silent passes.
    for variable, fallback in [('RUSTC', 'rustc'), ('CC', 'cc')]:
        executable = shlex.split(os.environ.get(variable, fallback))[0]
        if shutil.which(executable) is None:
            print(f'1..0 # SKIP missing host prerequisite: {variable}={executable}')
            return 4
    verify_fixtures()
    output = args.output or (Path(os.environ['ASAHI_M4_OUTPUT']) if os.environ.get('ASAHI_M4_OUTPUT')
                             else Path(tempfile.mkdtemp(prefix='asahi-m4-suite-')))
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tests = sorted(HERE.glob('test-*.py'))
    print('TAP version 13', flush=True)
    print(f'1..{len(tests)}', flush=True)
    results = []
    for number, test in enumerate(tests, 1):
        directory = output / test.stem
        directory.mkdir(exist_ok=True)
        environment = os.environ.copy()
        environment['ASAHI_M4_OUTPUT'] = str(directory)
        environment['ASAHI_M4_KERNEL'] = str(root)
        environment['PYTHONDONTWRITEBYTECODE'] = '1'
        command = [sys.executable, str(test)]
        # Resource allocator predates the shared output environment option.
        if test.name == 'test-resources.py':
            command += ['--output', str(directory)]
        start = time.monotonic()
        try:
            result = subprocess.run(command, env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=180)
            status, log = result.returncode, result.stdout
        except subprocess.TimeoutExpired as error:
            status = 124
            log = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) else (error.stdout or '')
            log += '\nHost regression exceeded 180 second limit\n'
        (directory / 'runner.log').write_text(log)
        passed, skipped = status == 0, status == 4
        label = 'ok' if passed or skipped else 'not ok'
        suffix = ' # SKIP prerequisite unavailable' if skipped else ''
        print(f'{label} {number} - {test.name}{suffix}', flush=True)
        for line in log.splitlines():
            print('# ' + line, flush=True)
        results.append({'test': test.name, 'returncode': status, 'seconds': round(time.monotonic()-start, 3), 'source_sha256': sha(test)})
    report = {'cpu_only': True, 'hardware_execution': False, 'passed': all(x['returncode'] in (0, 4) for x in results),
              'kernel_source': str(root), 'fixture_manifest_sha256': sha(HERE / 'fixtures/manifest.json'), 'tests': results}
    (output / 'host-tests.json').write_text(json.dumps(report, indent=2)+'\n')
    print('# Results: ' + str(output / 'host-tests.json'), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
