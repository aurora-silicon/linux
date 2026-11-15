#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Opt-in M4 common appledrm page-flip regression; default is a dry run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import stat
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def root_json(path):
    path = Path(path)
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid or info.st_mode & 0o022:
        raise RuntimeError('Expected root-owned regular manifest: ' + str(path))
    return json.loads(path.read_text())


def capture(command):
    return subprocess.check_output(command, text=True, timeout=15).strip()


def preflight(args):
    if os.geteuid() or os.uname().release != args.expected_release:
        raise RuntimeError('Hardware execution needs root and the exact candidate release')
    if not re.fullmatch('[0-9a-f]{40}', args.expected_source):
        raise RuntimeError('Expected full source commit')
    if not re.fullmatch('[0-9a-f]{64}', args.expected_image_sha256):
        raise RuntimeError('Expected complete Image SHA256')
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if boot != args.expected_boot_id:
        raise RuntimeError('Boot ID changed')
    words = shlex.split(Path('/proc/cmdline').read_text())
    for key, value in [('m4_asahi_rust', '1'), ('m4_asahi_rust.source', args.expected_source),
                       ('m4_asahi_rust.image', args.expected_image_sha256)]:
        if [w for w in words if w.startswith(key + '=')] != [key + '=' + value]:
            raise RuntimeError('Missing exact candidate command-line proof: ' + key)
    state_path = Path('/usr/lib/m4_asahi_rust/source-state.json')
    state = root_json(state_path)
    if not state.get('ready') or state.get('source_commit') != args.expected_source:
        raise RuntimeError('Installed source manifest is not the requested candidate')
    image = Path('/usr/lib/linux-m4-asahi-rust/Image')
    if sha(image) != args.expected_image_sha256 or state['artifact_sha256'].get(str(image)) != args.expected_image_sha256:
        raise RuntimeError('Image does not match exact candidate identity')
    module = Path(capture(['modinfo', '-n', 'appledrm'])).resolve(strict=True)
    pinned = [value for path, value in state['artifact_sha256'].items()
              if Path(path).resolve() == module]
    if len(pinned) != 1 or sha(module) != pinned[0]:
        raise RuntimeError('Common appledrm module does not match the frozen candidate')
    if Path('/sys/module/apple_dcp_m4_ready').exists():
        raise RuntimeError('Legacy standalone DCP owner is loaded')
    cards = [p.name for p in Path('/sys/class/drm').glob('card[0-9]*')
             if '-' not in p.name and (p / 'device/driver/module').exists()
             and (p / 'device/driver/module').resolve().name == 'appledrm']
    if len(cards) != 1:
        raise RuntimeError('Expected one DRM card owned by common appledrm')
    guard = dict(line.split('=', 1) for line in capture([
        'systemctl', 'show', 'm4-asahi-rust-rollback.service',
        '--property=ActiveState', '--property=MainPID', '--property=StatusText',
        '--property=ExecStart', '--property=DropInPaths', '--property=Environment']).splitlines())
    if (guard.get('ActiveState') != 'active' or not int(guard.get('MainPID', '0'))
            or 'unqualified' not in guard.get('StatusText', '').lower()
            or '/usr/lib/m4_asahi_rust/rollback-guard.py' not in guard.get('ExecStart', '')
            or guard.get('DropInPaths') or guard.get('Environment')
            or Path('/run/m4_asahi_rust-qualified').exists()):
        raise RuntimeError('Independent unqualified-candidate rollback guard must remain armed')
    desktop = capture(['systemctl', 'show', 'display-manager.service', '--property=ActiveState', '--value'])
    if desktop not in ('inactive', 'failed'):
        raise RuntimeError('Qualification operator must first stop the desktop display manager')
    return {'source_commit': args.expected_source, 'boot_id': boot,
            'release': args.expected_release, 'image_sha256': sha(image),
            'appledrm_module_sha256': sha(module), 'card': cards[0],
            'source_manifest_sha256': sha(state_path), 'rollback_guard': guard}


def compile_helper(output):
    source = HERE / 'native-m4-kms.c'
    flags = shlex.split(capture(['pkg-config', '--cflags', '--libs', 'libdrm']))
    binary = output / 'native-m4-kms'
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(source),
        '-o', str(binary), *flags]
    result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    (output / 'compile.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    proof = {'command': command, 'source_sha256': sha(source), 'binary_sha256': sha(binary),
             'runner_sha256': sha(__file__)}
    (output / 'build.json').write_text(json.dumps(proof, indent=2) + '\n')
    return binary, proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--execute', action='store_true')
    mode.add_argument('--compile-only', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--expected-source')
    parser.add_argument('--expected-boot-id')
    parser.add_argument('--expected-image-sha256')
    parser.add_argument('--expected-release', default='7.1.12-m4_asahi_rust')
    args = parser.parse_args()
    if not args.execute and not args.compile_only:
        print(json.dumps({'state': 'dry_run', 'hardware_access': False,
            'scope': 'shared appledrm eDP, 60 flips, CRTC-only atomic no-op, 20 blank/unblank cycles',
            'requires': 'explicit --execute, exact fresh boot/source/Image, armed rollback guard, desktop stopped'}, indent=2))
        return 0
    if not args.output or args.output.exists():
        parser.error('--output must name a fresh directory')
    if args.execute and not all([args.expected_source, args.expected_boot_id, args.expected_image_sha256]):
        parser.error('Execution requires exact source, boot ID and Image SHA256')
    identity = preflight(args) if args.execute else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    binary, proof = compile_helper(args.output)
    if args.compile_only:
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
        assert result.returncode == 2 and 'requires --execute' in result.stdout
        print(json.dumps({'compiled': True, 'hardware_access': False, 'build': proof}, indent=2))
        return 0
    if preflight(args) != identity:
        raise RuntimeError('Candidate or rollback guard changed during compilation')
    report = {'passed': False, 'identity': identity, 'build': proof, 'hardware_execution': True}
    child = None
    started = time.monotonic()
    try:
        with (args.output / 'kms.log').open('w+') as log:
            child = subprocess.Popen([str(binary), '--execute'], stdout=log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=120)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
                raise RuntimeError('KMS regression exceeded 120 seconds; rollback guard remains armed')
            log.seek(0)
            result = log.read()
        print(result, end='')
        report['returncode'] = code
        if code or 'PASS: native eDP, 60 matching page flips, CRTC-only commit, 20 blank/unblank cycles' not in result:
            raise RuntimeError('Native KMS regression failed; inspect kms.log')
        report['passed'] = True
    finally:
        report['elapsed_seconds'] = time.monotonic() - started
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
