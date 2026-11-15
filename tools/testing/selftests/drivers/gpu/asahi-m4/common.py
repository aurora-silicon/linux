# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Source discovery, compiler selection and immutable reference fixture checks."""
from functools import lru_cache
import gzip
import hashlib
import json
import os
from pathlib import Path
import shlex
import tempfile

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / 'fixtures'


def kernel_root():
    configured = os.environ.get('ASAHI_M4_KERNEL')
    if not configured and len(HERE.parents) <= 5:
        raise RuntimeError('Set ASAHI_M4_KERNEL to the kernel source tree being tested')
    root = (Path(configured) if configured else HERE.parents[5]).resolve()
    if not (root / 'drivers/gpu/drm/asahi/g16/mod.rs').is_file():
        raise RuntimeError('Set ASAHI_M4_KERNEL to the kernel source tree being tested')
    return root


def output_dir():
    selected = os.environ.get('ASAHI_M4_OUTPUT')
    path = Path(selected) if selected else Path(tempfile.mkdtemp(prefix='asahi-m4-host-'))
    path.mkdir(parents=True, exist_ok=True)
    return path


def rustc():
    return shlex.split(os.environ.get('RUSTC', 'rustc'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_fixtures():
    manifest = json.loads((FIXTURES / 'manifest.json').read_text())
    for name, entry in manifest['files'].items():
        path = FIXTURES / name
        if sha(path) != entry['sha256']:
            raise RuntimeError(f'Independent reference fixture changed: {name}')
    return manifest


@lru_cache(maxsize=1)
def frozen_bindings():
    # A comment edit to the UAPI header must not invalidate the host suite.
    # Compile both complete definitions and compare their ABI instead.
    from abi import c_layout, differences, rust_layout
    verify_fixtures()
    header = kernel_root() / 'include/uapi/drm/asahi_drm.h'
    bindings = FIXTURES / 'public-uapi.rs'
    with tempfile.TemporaryDirectory(prefix='asahi-m4-abi-') as name:
        output = Path(name)
        errors = differences(c_layout(header, bindings, output, 'kernel-layout'),
                             rust_layout(bindings, output))
    if errors:
        raise RuntimeError('Kernel UAPI ABI differs from frozen Rust bindings: ' + json.dumps(errors))
    return bindings


def unpack_fixtures(directory):
    manifest = verify_fixtures()
    directory.mkdir(parents=True, exist_ok=True)
    for name, entry in manifest['files'].items():
        if not name.endswith('.bin.gz'):
            continue
        data = gzip.decompress((FIXTURES / name).read_bytes())
        if len(data) != entry['uncompressed_bytes'] or hashlib.sha256(data).hexdigest() != entry['uncompressed_sha256']:
            raise RuntimeError(f'Uncompressed fixture changed: {name}')
        (directory / name[:-3]).write_bytes(data)
    return manifest


@lru_cache(maxsize=1)
def flat_bindings():
    """Materialize the current flat payloads from the frozen wire definitions.

    Encoder fixtures remain unchanged. The generated view merely folds the old
    prefix/extension declarations into their Linux 7.2 declaration shape.
    """
    import re
    source = frozen_bindings().read_text()
    names = ['drm_asahi_cmd_render', 'drm_asahi_cmd_render_with_prior',
             'drm_asahi_cmd_render_with_ray_query', 'drm_asahi_cmd_compute',
             'drm_asahi_cmd_compute_with_prior']
    bodies = {}
    for name in names:
        bodies[name] = re.search(r'pub struct ' + name + r' \{(.*?)^\}', source, re.M | re.S)[1]
    for base, extensions in [('drm_asahi_cmd_render', names[1:3]),
                             ('drm_asahi_cmd_compute', names[4:])]:
        body = bodies[base]
        for extension in extensions:
            body += re.sub(r'^    pub legacy:.*?\n', '', bodies[extension], flags=re.M)
        source = re.sub(r'(pub struct ' + base + r' \{).*?^\}',
                        lambda m: m[1] + body + '}', source, flags=re.M | re.S)
    for name in [names[1], names[2], names[4]]:
        source = re.sub(r'#\[repr\(C\)\]\n#\[derive\([^\n]*\)\]\npub struct ' + name + r' \{.*?^\}\n', '', source, flags=re.M | re.S)
    destination = output_dir() / 'flat-uapi.rs'
    destination.write_text(source)
    return destination
