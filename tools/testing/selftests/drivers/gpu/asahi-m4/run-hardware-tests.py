#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Explicit native OpenGL 4.6 test on a running M4 Rust Asahi kernel."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile
from common import HERE, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', type=Path, required=True, help='Installed paired native Mesa prefix')
    parser.add_argument('--render-node', type=Path, default=Path('/dev/dri/renderD128'))
    parser.add_argument('--rounds', type=int, default=32)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10000:
        parser.error('--rounds must be 1..10000')
    if not str(args.mesa) or str(args.mesa) == '.':
        parser.error('--mesa must name the installed paired native Mesa prefix')
    mesa = args.mesa.resolve(strict=True)
    library = mesa / 'lib'
    gallium = sorted(library.glob('libgallium-*.so'))
    if len(gallium) != 1 or not (library / 'libEGL_mesa.so.0').is_file():
        parser.error('Mesa prefix lacks one native Gallium library and libEGL_mesa.so.0')
    node = args.render_node.resolve(strict=True)
    device = (Path('/sys/class/drm') / node.name / 'device').resolve(strict=True)
    owners = [p / 'driver' for p in [device, *device.parents] if (p / 'driver').is_symlink()]
    if not any(p.resolve().name in ('aurora-g16', 'asahi-g16') for p in owners):
        parser.error('Selected render node is not owned by the Rust G16 platform driver (aurora-g16)')
    output = (args.output or Path(tempfile.mkdtemp(prefix='asahi-m4-hardware-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    for key in ['MESA_GL_VERSION_OVERRIDE', 'MESA_GLSL_VERSION_OVERRIDE', 'MESA_GLES_VERSION_OVERRIDE',
                'MESA_EXTENSION_OVERRIDE', 'LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER',
                'MESA_LOADER_DRIVER_OVERRIDE', 'DRI_PRIME', 'LD_PRELOAD', 'LD_AUDIT',
                '__EGL_VENDOR_LIBRARY_DIRS', '__EGL_VENDOR_LIBRARY_FILENAMES']:
        environment.pop(key, None)
    environment.update({'LD_LIBRARY_PATH': str(library), 'LIBGL_DRIVERS_PATH': str(library / 'dri'),
                        'GBM_BACKENDS_PATH': str(library / 'gbm'), 'M4_TEST_ROUNDS': str(args.rounds),
                        'PKG_CONFIG_PATH': str(library / 'pkgconfig') + ':' + str(mesa / 'share/pkgconfig')})
    vendor = mesa / 'share/glvnd/egl_vendor.d/50_mesa.json'
    if vendor.is_file():
        environment['__EGL_VENDOR_LIBRARY_FILENAMES'] = str(vendor)
    flags = subprocess.check_output(shlex.split(os.environ.get('PKG_CONFIG', 'pkg-config')) +
                                    ['--cflags', '--libs', 'egl', 'gbm', 'gl'], env=environment, text=True)
    binary = output / 'native-gl46'
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-O2', '-Wall', '-Wextra', '-Werror', str(HERE / 'native-gl46.c'), '-o', str(binary)] + shlex.split(flags)
    subprocess.run(command, env=environment, check=True)
    result = subprocess.run([str(binary), str(node)], env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=900)
    print(result.stdout, end='')
    (output / 'native-gl46.log').write_text(result.stdout)
    report = {'passed': result.returncode == 0, 'scope': 'Native OpenGL rendering, compute and geometry regression; not Vulkan conformance or complete driver qualification.',
              'kernel_release': platform.release(), 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
              'render_node': str(node), 'mesa_prefix': str(mesa), 'rounds': args.rounds,
              'sha256': {str(HERE / 'native-gl46.c'): sha(HERE / 'native-gl46.c'), str(binary): sha(binary), str(gallium[0]): sha(gallium[0])}}
    (output / 'hardware-tests.json').write_text(json.dumps(report, indent=2)+'\n')
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
