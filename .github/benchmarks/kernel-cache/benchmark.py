#!/usr/bin/env python3
"""Matched ARM64 kernel compile with a fully warmed C/C++ compiler cache.

The first full build warms ccache. Its output tree is retained separately.
The measured second build starts with a fresh output tree at the same path,
and reuses only ccache. Rust and all final links run on both machines.
No target executables, emulators, guests, or QEMU are run.
"""
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import urllib.request

assert __debug__, 'validation must remain enabled'
INPUT = Path(__file__).resolve().parent
MANIFEST = json.loads((INPUT / 'manifest.json').read_text())
ROOT = Path(MANIFEST['root'])
SOURCE = ROOT / 'linux'
OUT = ROOT / 'out'
RESULTS = ROOT / 'results'
CACHE = ROOT / 'ccache'
TOOL = ROOT / 'bindgen'
MACHINE = os.environ.get('BENCHMARK_MACHINE', 'server')
JOBS = len(os.sched_getaffinity(0))
FLOOR = 8 * 1024**3
sha = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()

def output(*cmd, env=None):
    return subprocess.check_output(cmd, text=True, env=env).strip()

def logrun(name, cmd, env=None, cwd=None):
    path = RESULTS / (name + '.log')
    assert not path.exists(), 'refusing to overwrite ' + str(path)
    print(json.dumps({'stage': name, 'command': cmd}), flush=True)
    with path.open('w') as log:
        subprocess.run(cmd, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

def cache_stats(env):
    text = output('ccache', '--print-stats', env=env)
    return {line.split()[0]: int(line.split()[1]) for line in text.splitlines()
            if len(line.split()) == 2 and line.split()[1].isdigit()}

def configure(label, env):
    OUT.mkdir()
    shutil.copyfile(INPUT / 'kernel.config', OUT / '.config')
    # Reuse this host's generated signing key so fresh outputs retain identical
    # trust material. Private keys stay on their originating machine.
    retained_key = ROOT / 'out-warmup/certs/signing_key.pem'
    if retained_key.exists():
        (OUT / 'certs').mkdir()
        # Kbuild also needs the saved generation command and its prerequisite;
        # otherwise FORCE would generate a new key despite an existing PEM.
        for name in ('x509.genkey', '.signing_key.pem.cmd', 'signing_key.pem'):
            shutil.copy2(retained_key.parent / name, OUT / 'certs' / name)
        (OUT / 'certs/signing_key.pem').chmod(0o600)
    logrun(label + '-config', ['make', '-C', str(SOURCE), 'O=' + str(OUT),
           'ARCH=arm64', 'LLVM=-19', 'BINDGEN=' + str(TOOL / 'bin/bindgen'),
           'olddefconfig'], env=env)
    assert sha(OUT / '.config') == MANIFEST['config_sha256'], 'resolved configuration differs'

def measured(label, env):
    record_path = RESULTS / (label + '.json')
    assert not record_path.exists(), 'refusing to overwrite prior build evidence'
    time_path = RESULTS / (label + '-time.txt')
    command = ['/usr/bin/time', '-v', '-o', str(time_path),
               'nice', '-n', '0', 'ionice', '-c2', '-n4',
               'make', '-C', str(SOURCE), 'O=' + str(OUT),
               'ARCH=arm64', 'LLVM=-19', 'BINDGEN=' + str(TOOL / 'bin/bindgen'),
               '-j' + str(JOBS), *MANIFEST['targets']]
    before = cache_stats(env)
    start = time.monotonic()
    peak_rss = 0
    minimum_free = shutil.disk_usage(ROOT).free
    page_size = os.sysconf('SC_PAGE_SIZE')
    print(json.dumps({'stage': label, 'event': 'start', 'command': command}), flush=True)
    with (RESULTS / (label + '.log')).open('w') as log, (RESULTS / (label + '-metrics.jsonl')).open('w') as metrics:
        proc = subprocess.Popen(command, env=env, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
        tick = 0
        while True:
            rss = 0
            for entry in Path('/proc').iterdir():
                if not entry.name.isdigit():
                    continue
                try:
                    fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
                    if int(fields[2]) == proc.pid:
                        rss += int(fields[21]) * page_size
                except (OSError, ValueError, IndexError):
                    pass
            free = shutil.disk_usage(ROOT).free
            peak_rss = max(peak_rss, rss)
            minimum_free = min(minimum_free, free)
            sample = {'stage': label, 'elapsed': round(time.monotonic() - start, 1),
                      'free_bytes': free, 'process_rss_bytes': rss}
            metrics.write(json.dumps(sample) + '\n')
            metrics.flush()
            if tick % 3 == 0:
                print(json.dumps(sample), flush=True)
            if proc.poll() is not None:
                break
            if free < FLOOR + 2 * 1024**3:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                raise RuntimeError('Stopped only this build to preserve the 8 GiB free-space floor')
            time.sleep(10)
            tick += 1
    report = time_path.read_text()
    wall = re.search(r'Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s+(\S+)', report).group(1)
    seconds = 0.0
    for part in wall.split(':'):
        seconds = seconds * 60 + float(part)
    after = cache_stats(env)
    record = {'machine': MACHINE, 'stage': label, 'source_commit': MANIFEST['source_commit'],
              'config_sha256': sha(OUT / '.config'), 'command': command,
              'exit_code': proc.wait(), 'wall_clock': wall, 'seconds': seconds,
              'jobs': JOBS, 'peak_sampled_process_rss_bytes': peak_rss,
              'minimum_free_bytes': minimum_free,
              'ccache_before': before, 'ccache_after': after,
              'ccache_delta': {k: after[k] - before.get(k, 0) for k in after},
              'qemu_used': False, 'fresh_output_tree': label != 'unchanged'}
    if record['exit_code'] == 0:
        record.update(image_sha256=sha(OUT / 'arch/arm64/boot/Image'),
                      module_count=len(list(OUT.rglob('*.ko'))),
                      dtb_count=len(list((OUT / 'arch/arm64/boot/dts').rglob('*.dtb'))))
    record_path.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    assert record['exit_code'] == 0, label + ' failed; retained complete log and output tree'
    return record

assert platform.machine() == 'x86_64', 'this benchmark requires native x86_64 hosts'
ROOT.mkdir(parents=True, exist_ok=True)
RESULTS.mkdir(exist_ok=True)
lock_path = Path('/srv/aurora-cross-build/kernel-build.lock')
if not lock_path.parent.exists():
    lock_path = ROOT / 'kernel-build.lock'
lock = lock_path.open('a')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
assert not (RESULTS / 'environment.json').exists(), 'refusing to replace a retained benchmark'
assert shutil.disk_usage(ROOT).free >= 40 * 1024**3, 'need 40 GiB free for two retained output trees, cache, sources, and 8 GiB floor'
assert sha(INPUT / 'kernel.config') == MANIFEST['config_sha256']
packages = {}
for name, expected in MANIFEST['expected_packages'].items():
    actual = output('dpkg-query', '-W', '-f=${Version}', name)
    assert actual == expected, f'{name}: expected {expected}, got {actual}'
    packages[name] = actual
environment = {'machine': MACHINE, 'manifest': MANIFEST, 'jobs': JOBS,
               'packages': packages, 'uname': output('uname', '-a'),
               'lscpu': output('lscpu'), 'cpu_affinity': sorted(os.sched_getaffinity(0)),
               'runner_name': os.environ.get('RUNNER_NAME'),
               'github_run_id': os.environ.get('GITHUB_RUN_ID'),
               'benchmark_script_sha256': sha(Path(__file__)),
               'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
(RESULTS / 'environment.json').write_text(json.dumps(environment, indent=2) + '\n')
assert not SOURCE.exists() and not OUT.exists() and not TOOL.exists(), 'benchmark paths already exist'
SOURCE.mkdir()
logrun('git-init', ['git', 'init', str(SOURCE)])
logrun('git-fetch', ['git', '-C', str(SOURCE), 'fetch', '--depth=1', MANIFEST['source_url'], MANIFEST['source_commit']])
logrun('git-checkout', ['git', '-C', str(SOURCE), 'checkout', '--detach', 'FETCH_HEAD'])
assert output('git', '-C', str(SOURCE), 'rev-parse', 'HEAD') == MANIFEST['source_commit']

env = dict(os.environ, PATH=str(ROOT / 'ccache-bin') + ':/usr/lib/llvm-19/bin:' + os.environ['PATH'],
           CCACHE_DIR=str(CACHE), CCACHE_BASEDIR=str(SOURCE), LIBCLANG_PATH='/usr/lib/llvm-19/lib',
           RUST_LIB_SRC='/usr/lib/rustlib/src/rust/library', KBUILD_BUILD_USER='aurora-benchmark',
           KBUILD_BUILD_HOST='kernel-builder', KBUILD_BUILD_VERSION='1',
           KBUILD_BUILD_TIMESTAMP=output('git', '-C', str(SOURCE), 'show', '-s', '--format=%cD', 'HEAD'))
CACHE.mkdir(exist_ok=True)
(ROOT / 'ccache-bin').mkdir()
for name in ('clang-19', 'clang++-19'):
    (ROOT / 'ccache-bin' / name).symlink_to('/usr/bin/ccache')
subprocess.run(['ccache', '--set-config=max_size=' + MANIFEST['cache_max_size']], env=env, check=True)
subprocess.run(['ccache', '--set-config=compiler_check=content'], env=env, check=True)
subprocess.run(['ccache', '--set-config=compression=true'], env=env, check=True)

# Build the pinned, layout-corrected native generator. The documented adapter
# removes only a lint allowance unknown to Rust 1.85; all layout checks remain.
TOOL.mkdir()
tool_source = SOURCE / 'tools/aurora-bindgen'
pin = json.loads((tool_source / 'sources.json').read_text())
archive = TOOL / 'source.tar.gz'
with urllib.request.urlopen(pin['source_url'], timeout=60) as response, archive.open('wb') as dest:
    shutil.copyfileobj(response, dest)
assert sha(archive) == pin['source_sha256']
with tarfile.open(archive) as tar:
    tar.extractall(TOOL, filter='data')
bindgen_source = TOOL / pin['source_directory']
for name, digest in pin['patches'].items():
    patch = tool_source / 'patches' / name
    assert sha(patch) == digest
    logrun('patch-' + name, ['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)], cwd=bindgen_source)
assert sha(bindgen_source / 'Cargo.lock') == pin['cargo_lock_sha256']
for name, digest in pin['patched_files'].items():
    assert sha(bindgen_source / name) == digest
logrun('bindgen-build', ['cargo', 'build', '--locked', '--release', '-p', 'bindgen-cli', '-j2'], env=env, cwd=bindgen_source)
(TOOL / 'bin').mkdir()
(TOOL / 'bin/native-bindgen').symlink_to(bindgen_source / 'target/release/bindgen')
shutil.copyfile(INPUT / 'bindgen-adapter.py', TOOL / 'bin/bindgen')
(TOOL / 'bin/bindgen').chmod(0o755)
logrun('bindgen-layout-tests', [sys.executable, str(tool_source / 'test.py'), '--bindgen', str(TOOL / 'bin/bindgen'),
       '--output-dir', str(TOOL / 'tests')], env=env)
(RESULTS / 'tool-qualification.json').write_text(json.dumps({
    'binary_sha256': sha(TOOL / 'bin/native-bindgen'),
    'adapter_sha256': sha(TOOL / 'bin/bindgen'),
    'tests_manifest': json.loads((TOOL / 'tests/result.json').read_text()),
    'rustc': output('rustc', '-vV'), 'clang': output('clang-19', '--version')}, indent=2) + '\n')

configure('warmup', env)
warmup = measured('warmup', env)
assert not (ROOT / 'out-warmup').exists()
OUT.rename(ROOT / 'out-warmup')  # Preserve it; recreate the SAME canonical path.
configure('cached', env)
cached = measured('cached', env)
assert cached['image_sha256'] == warmup['image_sha256'], 'fresh and compiler-cache builds changed the image bytes'
unchanged = measured('unchanged', env)
assert not output('git', '-C', str(SOURCE), 'status', '--porcelain')
(RESULTS / 'summary.json').write_text(json.dumps({'environment': environment,
    'warmup': warmup, 'cached': cached, 'unchanged': unchanged,
    'interpretation': 'Compare cached.seconds between machines. Both measured builds start with a fresh output tree and a compiler cache populated by the same full build. Rust is rebuilt on both; ccache covers C/C++.'}, indent=2) + '\n')
print('BENCHMARK_COMPLETE ' + str(RESULTS / 'summary.json'), flush=True)
