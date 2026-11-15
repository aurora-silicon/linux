#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Opt-in live M4 admission/PPL regression; dry run unless --execute is given."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import stat
import subprocess
import threading
import time

HERE = Path(__file__).resolve().parent
RELEASE = '7.1.12-m4_asahi_rust'
UNPATCHED = {'78d2519c82fca4c0403315e040d595628bc5a4d6',
             'df583754ae2b3607af4155ba0442bc93077bdce8'}
FAULT = re.compile(r'Kernel panic|BUG:|WARNING: CPU|SError|Oops:|Unable to handle kernel|'
                   r'\b(?:aurora|asahi|G16|GPU|PPL)\b.*(?:fault|fail(?:ed|ure)?|timeout|timed out|quarantin)', re.I)
# The G16 platform driver and its debugfs directory; kernels before the Aurora
# rename used the asahi-g16 name.
G16_DRIVERS = ('aurora-g16', 'asahi-g16')
G16_STATS = [Path('/sys/kernel/debug') / name / 'stats' for name in G16_DRIVERS]
EXPECTED_REJECTION = re.compile(r'IOCTL: submit failed! \(submission ID: \d+ err: E2BIG\)\s*$')


def kernel_fault(line):
    # Strip only the exact rejection this test deliberately provokes. Keep the
    # prefix, so an independent fault/panic on the same line is never hidden.
    return FAULT.search(EXPECTED_REJECTION.sub('', line)) is not None


def self_test():
    expected = 'asahi-g16 302600000.gpu: [File 10 Queue 2]: IOCTL: submit failed! (submission ID: 42 err: E2BIG)'
    harmless = [expected, expected + '\n', expected.removeprefix('asahi-g16 302600000.gpu: '),
                'apple-dcp 231c00000.dcp: mode validation failed',
                'apple-smc: mailbox timeout', 'asahi-g16: completed compute']
    failures = ['asahi-g16: G16: publication of ctx 4 failed after visibility began (EIO)',
                'PPL: host handshake timeout',
                'asahi::mmu: retaining M4 mapping after PPL failure: ETIMEDOUT',
                'Kernel panic - not syncing: ' + expected,
                expected + ' GPU fault bank 0',
                expected.replace('err: E2BIG', 'err: EIO'),
                'asahi-g16: GPU fault bank 0; ' + expected,
                'aurora-g16: GPU fault bank 0; ' + expected]
    for line in harmless:
        assert not kernel_fault(line), 'False fault match: ' + line
    for line in failures:
        assert kernel_fault(line), 'Missed real fault: ' + line
    print(json.dumps({'passed': True, 'harmless_messages': len(harmless),
                      'fault_messages': len(failures), 'hardware_access': False,
                      'runner_sha256': sha(__file__)}, indent=2))


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def root_json(path):
    path = Path(path)
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid or info.st_mode & 0o022:
        raise RuntimeError('Expected a root-owned immutable regular manifest: ' + str(path))
    return json.loads(path.read_text())


def preflight(args):
    if os.geteuid() or os.uname().release != args.expected_release:
        raise RuntimeError('Execution requires root and the exact candidate kernel release')
    if args.expected_source in UNPATCHED or not re.fullmatch('[0-9a-f]{40}', args.expected_source):
        raise RuntimeError('Refusing known unpatched source or an invalid source identity')
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if boot != args.expected_boot_id:
        raise RuntimeError('Unexpected kernel boot ID')
    cmdline = shlex.split(Path('/proc/cmdline').read_text())
    for key, value in [('m4_asahi_rust', '1'), ('m4_asahi_rust.source', args.expected_source),
                       ('m4_asahi_rust.image', args.expected_image_sha256)]:
        if [word for word in cmdline if word.startswith(key + '=')] != [key + '=' + value]:
            raise RuntimeError('Running kernel command line lacks the exact candidate ' + key)
    source_path = Path('/usr/lib/m4_asahi_rust/source-state.json')
    state = root_json(source_path)
    if not state.get('ready') or state.get('source_commit') != args.expected_source or state.get('kernel_release') != args.expected_release:
        raise RuntimeError('Installed source-state does not match the patched candidate')
    image = Path('/usr/lib/linux-m4-asahi-rust/Image')
    if state['artifact_sha256'].get(str(image)) != args.expected_image_sha256 or sha(image) != args.expected_image_sha256:
        raise RuntimeError('Installed Image differs from the exact running candidate')
    module = Path(subprocess.check_output(['modinfo', '-n', 'asahi'], text=True).strip()).resolve(strict=True)
    module_keys = [name for name in state['artifact_sha256'] if Path(name).resolve() == module]
    if len(module_keys) != 1 or sha(module) != state['artifact_sha256'][module_keys[0]]:
        raise RuntimeError('Installed Asahi module differs from the frozen candidate')
    node = args.render_node.resolve(strict=True)
    if not stat.S_ISCHR(node.stat().st_mode):
        raise RuntimeError('Expected a DRM character device')
    device = Path('/sys/class/drm') / node.name / 'device'
    owner = (device / 'driver').resolve(strict=True).name
    if owner not in G16_DRIVERS:
        raise RuntimeError('The Rust G16 platform driver does not own the requested render node')
    if any((Path('/sys/module') / name).exists() for name in ['gpu_m4_drm', 'apple_gpu_m4_power']):
        raise RuntimeError('Legacy C GPU provider is loaded')
    active = root_json('/run/aurorasilicon/m4-asahi-rust.json')
    if (active.get('state') != 'ready' or active.get('source_commit') != args.expected_source or
            active.get('kernel_release') != args.expected_release or Path(active['render']).resolve() != node):
        raise RuntimeError('Exact patched native desktop activation is not ready')
    unit_text = subprocess.check_output(['systemctl', 'show', 'm4-asahi-rust-rollback.service',
                                        '--property=ActiveState', '--property=MainPID',
                                        '--property=ExecStart', '--property=DropInPaths',
                                        '--property=Environment', '--property=StatusText'], text=True)
    guard = dict(line.split('=', 1) for line in unit_text.splitlines() if '=' in line)
    if (guard.get('ActiveState') != 'active' or not int(guard.get('MainPID', '0')) or
            guard.get('DropInPaths') or guard.get('Environment') or
            '/usr/lib/m4_asahi_rust/rollback-guard.py' not in guard.get('ExecStart', '') or
            'unqualified' not in guard.get('StatusText', '').lower()):
        raise RuntimeError('The independent unqualified-candidate rollback supervisor must be armed')
    if Path('/run/m4_asahi_rust-qualified').exists():
        raise RuntimeError('Qualification marker already exists; rollback supervisor may disarm')
    return {'boot_id': boot, 'source_commit': args.expected_source, 'kernel_release': os.uname().release,
            'kernel_image_sha256': sha(image), 'asahi_module_sha256': sha(module),
            'source_state_sha256': sha(source_path), 'render_node': str(node), 'owner': owner,
            'rollback_guard': guard, 'activation': active}


def compile_helper(args, output):
    header_dir = args.uapi_dir
    if header_dir is None:
        root = Path(os.environ.get('ASAHI_M4_KERNEL', HERE.parents[5]))
        header_dir = root / 'include/uapi/drm'
    header_dir = header_dir.resolve(strict=True)
    headers = [header_dir / name for name in ['asahi_drm.h', 'drm.h', 'drm_mode.h']]
    header_hashes = {str(path): sha(path) for path in headers}
    if args.expected_uapi_sha256 and header_hashes[str(headers[0])] != args.expected_uapi_sha256:
        raise RuntimeError('Selected actual kernel UAPI header does not match the pinned source')
    binary = output / 'native-ioctl-regression'
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
        # Raw source UAPI annotations are removed by headers_install; no ABI changes.
        '-D__user=', '-I', str(header_dir),
        '-DASAHI_M4_UAPI_SHA256="' + header_hashes[str(headers[0])] + '"',
        str(HERE / 'native-ioctl-regression.c'), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=60)
    proof = {'compiled_utc': utc(), 'command': command, 'binary_sha256': sha(binary),
             'header_sha256': header_hashes, 'asahi_uapi_sha256': header_hashes[str(headers[0])],
             'test_source_sha256': sha(HERE / 'native-ioctl-regression.c'),
             'runner_sha256': sha(__file__), 'hardware_execution': False}
    (output / 'native-ioctl-build.json').write_text(json.dumps(proof, indent=2) + '\n')
    return binary, proof


def snapshot():
    stats = next(p for p in G16_STATS if p.exists()).read_text()
    published = re.search(r'^publications: compute (\d+) render (\d+)\s*$', stats, re.M)
    if not published:
        raise RuntimeError('Missing actual firmware-producer publication counters; refuse live regression')
    counters = {}
    for key in ['recoveries', 'hang_kills']:
        match = re.search(r'\b' + key + r' (\d+)\b', stats)
        if not match:
            raise RuntimeError('Missing driver health counter: ' + key)
        counters[key] = int(match[1])
    match = re.search(r'^pb:.*\bbad (\d+)\b', stats, re.M)
    if not match:
        raise RuntimeError('Missing PB health counter')
    counters['pb_bad'] = int(match[1])
    return {'utc': utc(), 'stats': stats, 'counters': counters,
            'publications': {'compute': int(published[1]), 'render': int(published[2])},
            'meminfo': Path('/proc/meminfo').read_text()}


def terminate(process):
    if process is not None and process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--execute', action='store_true')
    mode.add_argument('--compile-only', action='store_true', help='Build on this CPU; no DRM or target access')
    mode.add_argument('--self-test', action='store_true', help='CPU-only fault-message classification checks')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--uapi-dir', type=Path, help='Actual source include/uapi/drm directory; never mirrored constants')
    parser.add_argument('--render-node', type=Path, default=Path('/dev/dri/renderD128'))
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--loops', type=int, default=32)
    parser.add_argument('--expected-release', default=RELEASE)
    parser.add_argument('--expected-source')
    parser.add_argument('--expected-boot-id')
    parser.add_argument('--expected-image-sha256')
    parser.add_argument('--expected-uapi-sha256')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.threads not in [2, 4, 6, 8] or not 1 <= args.loops <= 256:
        parser.error('--threads must be 2/4/6/8 and --loops 1..256')
    if not args.execute and not args.compile_only:
        print(json.dumps({'state': 'dry_run', 'GPU_ioctls': False, 'target_writes': False,
                          'mixed_rejections': 4, 'stress_threads': args.threads,
                          'stress_loops_per_thread': args.loops, 'shader_dispatches': 0,
                          'requires': '--execute --output NEW --expected-source COMMIT --expected-boot-id UUID --expected-image-sha256 SHA --expected-uapi-sha256 SHA; exact patched source/Image boot tokens and active rollback supervisor'}, indent=2))
        return 0
    if not args.output or args.output.exists():
        parser.error('--output must name a fresh directory')
    if args.execute and not all([args.expected_source, args.expected_boot_id,
                                 args.expected_image_sha256, args.expected_uapi_sha256]):
        parser.error('Execution requires exact source, boot ID, Image and actual UAPI hashes')
    identity = preflight(args) if args.execute else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    binary, build = compile_helper(args, args.output)
    if args.compile_only:
        subprocess.run([str(binary)], check=True, timeout=5)
        print(json.dumps(build, indent=2))
        return 0
    # Recheck the guard and exact identity after compilation, before the first ioctl.
    if preflight(args) != identity:
        raise RuntimeError('Candidate identity changed while the helper was compiled')
    before = snapshot()
    if any(before['counters'].values()):
        raise RuntimeError('Driver health baseline already has errors')
    report = {'passed': False, 'started_utc': utc(), 'identity': identity, 'build': build,
              'before': before, 'scope': 'Live structural-admission ordering and bounded common-PPL allocation serialization; no shaders, intentional hangs, corruption injection or conformance claim.',
              'hardware_acceptance': False, 'shader_dispatches': 0}
    child = journal = None
    reader = None
    fault = threading.Event()

    def watch():
        with (args.output / 'kernel-follow.log').open('w') as stream:
            for line in journal.stdout:
                stream.write(line)
                stream.flush()
                if kernel_fault(line):
                    fault.set()

    try:
        journal = subprocess.Popen(['journalctl', '-k', '-b', '-f', '-n', '0', '-o', 'cat', '--no-pager'],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
        reader = threading.Thread(target=watch, daemon=True)
        reader.start()
        time.sleep(0.1)
        if journal.poll() is not None:
            raise RuntimeError('Kernel fault monitor could not start')
        command = [str(binary), '--execute', str(args.render_node), args.expected_release,
                   args.expected_boot_id, args.expected_source, args.expected_image_sha256,
                   args.expected_uapi_sha256, str(args.threads), str(args.loops)]
        report['command'] = command
        with (args.output / 'native-ioctl.log').open('w+') as log:
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 100
            while child.poll() is None:
                if fault.wait(0.05) or journal.poll() is not None:
                    raise RuntimeError('Kernel fault or loss of the kernel fault monitor')
                if time.monotonic() >= deadline:
                    raise RuntimeError('Bounded ioctl regression exceeded 100 seconds')
            report['returncode'] = child.returncode
            log.seek(0)
            text = log.read()
        print(text, end='')
        if child.returncode:
            raise RuntimeError('Live ioctl helper failed')
        result = json.loads(text.strip())
        report['result'] = result
        if (not result['passed'] or result['mixed_e2big_rejections'] != 4 or
                result['vm_create_destroy_cycles'] != args.threads // 2 * args.loops or
                result['timestamp_bind_unbind_cycles'] != args.threads // 2 * args.loops or
                result.get('simultaneous_timestamp_aliases') != 4097):
            raise RuntimeError('Incomplete live ioctl evidence')
        if fault.is_set():
            raise RuntimeError('Kernel fault observed')
        report['after'] = snapshot()
        if report['after']['counters'] != before['counters']:
            raise RuntimeError('GPU health counter changed')
        report['passed'] = True
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
        try:
            report['after_failure'] = snapshot()
        except Exception as snapshot_error:
            report['after_failure_error'] = str(snapshot_error)
    finally:
        terminate(child)
        terminate(journal)
        if reader:
            reader.join(timeout=2)
        if fault.is_set():
            report['passed'] = False
            report.setdefault('error', 'Kernel fault observed')
        report['ended_utc'] = utc()
        (args.output / 'native-ioctl-results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'error': report.get('error'), 'output': str(args.output)}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
