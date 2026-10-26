# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile ABI layouts, selecting declarations from generated Rust bindings.

Only the generated binding declaration names are enumerated. Every measurement
is evaluated by the C and Rust compilers from full, unmodified definitions;
production function bodies and C headers are never sliced or parsed here.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


def inventory(bindings):
    source = bindings.read_text()
    structures = {}
    for name, body in re.findall(r'^pub struct (drm_asahi_\w+) \{(.*?)^\}', source, re.M | re.S):
        structures[name] = re.findall(r'^    pub (\w+):', body, re.M)
    constants = {}
    for rust_name in re.findall(r'^pub const (\w+):', source, re.M):
        if rust_name.startswith('DRM_ASAHI_'):
            constants[rust_name] = rust_name
        elif '_DRM_ASAHI_' in rust_name:
            constants[rust_name] = 'DRM_ASAHI_' + rust_name.split('_DRM_ASAHI_', 1)[1]
    if not structures or not constants:
        raise ValueError('no Asahi UAPI declarations in generated Rust bindings')
    return structures, constants


def expressions(bindings):
    structures, constants = inventory(bindings)
    c, rust = [], []
    for name, fields in structures.items():
        for suffix, ce, rexp in [
            ('size', f'sizeof(struct {name})', f'core::mem::size_of::<uapi::{name}>()'),
            ('align', f'_Alignof(struct {name})', f'core::mem::align_of::<uapi::{name}>()')]:
            key = name + ':' + suffix
            c.append((key, ce))
            rust.append((key, rexp))
        for field in fields:
            key = name + ':' + field
            c.append((key, f'offsetof(struct {name}, {field})'))
            rust.append((key, f'core::mem::offset_of!(uapi::{name}, {field})'))
    for rust_name, c_name in constants.items():
        c.append((c_name, c_name))
        rust.append((c_name, 'uapi::' + rust_name))
    return c, rust


def run_measurements(command, source, binary):
    result = subprocess.run(command + [str(source), '-o', str(binary)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'ABI harness compile failed: {source}\n{result.stdout}')
    output = subprocess.check_output([str(binary)], text=True)
    return dict((key, int(value)) for key, value in
                (line.split('=', 1) for line in output.splitlines()))


def c_layout(header, bindings, output, tag):
    keys, _ = expressions(bindings)
    capacity_tail = 'client_memory_limit;' in header.read_text()
    # Linux 7.2 folds the historical extension structs into the base payloads.
    # Compare the same wire prefixes and offsets while keeping the C oracle frozen.
    if 'struct drm_asahi_cmd_render_with_prior {' not in header.read_text():
        translated = []
        for key, expr in keys:
            kind, _, field = key.partition(':')
            if capacity_tail and key == 'drm_asahi_params_global:size':
                # Compare the frozen reply prefix; validate the entire known additive tail below.
                expr = 'offsetof(struct drm_asahi_params_global, memory_limit)'
            if key == "drm_asahi_vm_create:pad":
                expr = "offsetof(struct drm_asahi_vm_create, flags)"
            base = None
            if kind.startswith('drm_asahi_cmd_render'):
                base = 'drm_asahi_cmd_render'
                sizes = {'drm_asahi_cmd_render': 'offsetof(struct drm_asahi_cmd_render, prior_vdm)',
                         'drm_asahi_cmd_render_with_prior': 'offsetof(struct drm_asahi_cmd_render, ray_query)',
                         'drm_asahi_cmd_render_with_ray_query': 'sizeof(struct drm_asahi_cmd_render)'}
            elif kind.startswith('drm_asahi_cmd_compute'):
                base = 'drm_asahi_cmd_compute'
                sizes = {'drm_asahi_cmd_compute': 'offsetof(struct drm_asahi_cmd_compute, scs_layout)',
                         'drm_asahi_cmd_compute_with_prior': 'sizeof(struct drm_asahi_cmd_compute)'}
            if base and kind in sizes:
                if field == 'size': expr = sizes[kind]
                elif field == 'align': expr = f'_Alignof(struct {base})'
                elif field == 'legacy': expr = '0'
                else: expr = f'offsetof(struct {base}, {field})'
            translated.append((key, expr))
        keys = translated
    source = output / (tag + '.c')
    lines = ['#include <stdio.h>', '#include <stddef.h>',
             '#include ' + json.dumps(str(header)), 'int main(void) {']
    if capacity_tail:
        lines[3:3] = [
            '_Static_assert(sizeof(struct drm_asahi_params_global) == 616, "capacity reply size");',
            '_Static_assert(offsetof(struct drm_asahi_params_global, memory_limit) == 600, "capacity prefix");',
            '_Static_assert(offsetof(struct drm_asahi_params_global, client_memory_limit) == 608, "client capacity offset");',
            '_Static_assert(sizeof(((struct drm_asahi_params_global *)0)->memory_limit) == 8, "capacity width");',
            '_Static_assert(sizeof(((struct drm_asahi_params_global *)0)->client_memory_limit) == 8, "client capacity width");',
        ]
    lines += [f'printf("{key}=%llu\\n", (unsigned long long)({expr}));' for key, expr in keys]
    source.write_text('\n'.join(lines + ['return 0;', '}']) + '\n')
    return run_measurements(shlex.split(os.environ.get('CC', 'cc')) +
                            ['-std=gnu11', '-Wall', '-Werror', '-D__user='],
                            source, output / tag)


def rust_layout(bindings, output):
    _, keys = expressions(bindings)
    source = output / 'rust-layout.rs'
    lines = ['#![allow(dead_code, non_camel_case_types, non_upper_case_globals)]',
             'mod uapi { include!(' + json.dumps(str(bindings)) + '); }', 'fn main() {']
    lines += [f'println!("{key}={{}}", ({expr}) as u64);' for key, expr in keys]
    source.write_text('\n'.join(lines + ['}']) + '\n')
    return run_measurements(shlex.split(os.environ.get('RUSTC', 'rustc')) + ['--edition=2021'],
                            source, output / 'rust-layout')


def differences(left, right):
    return [{'key': key, 'left': left.get(key), 'right': right.get(key)}
            for key in sorted(left.keys() | right.keys()) if left.get(key) != right.get(key)]
