#!/usr/bin/env python3
"""Assemble an installer from locally built, hash-matched stack artifacts."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROLES = {'kernel':'linux-aurora', 'headers':'linux-aurora-headers', 'm1n1':'m1n1-aurora',
         'mesa':'mesa-m3', 'libfprint':'libfprint', 'touchid':'aurora-touchid'}
HEX = re.compile(r'[0-9a-f]{64}')

def member(path, name):
    return subprocess.check_output(['bsdtar', '-xOf', str(path), name])

def desktop_data(manifest, directory):
    data = manifest.get('desktop_fixes')
    if data is None:
        return ''
    path = Path(__file__).with_name('desktop-fixes.py')
    spec = importlib.util.spec_from_file_location('desktop_fixes', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.validate_manifest(data)
    runtime = {}
    for name, item in data['packages'].items():
        module.verify_archive(name, item, directory)
        if name != 'aquamarine':
            for path, entry in module.package_contents(directory / item['file']).items():
                if module.immutable(path):
                    if path in runtime and runtime[path] != entry:
                        raise ValueError('paired desktop runtime files conflict')
                    runtime[path] = entry
    if runtime != data['omarchy_catalogs']['4.0.4-2']['files']:
        raise ValueError('guarded runtime catalog differs from the paired package payload')
    return base64.b64encode(json.dumps(data, sort_keys=True, separators=(',', ':')).encode()).decode()

def assemble(template, manifest, directory):
    if manifest.get('schema') != 'aurora.m3-matched-stack/1': raise ValueError('manifest schema differs')
    for field in ('version','tag'):
        if not re.fullmatch(r'[A-Za-z0-9._+-]+', manifest.get(field,'')): raise ValueError(f'invalid {field}')
    sources = manifest.get('source_commits', {})
    if any(not re.fullmatch(r'[0-9a-f]{40}', sources.get(k,'')) for k in ('kernel','m1n1')):
        raise ValueError('exact kernel and m1n1 source commits are required')
    boards = manifest.get('legacy_gpu_boards', ['j613'])
    if (not isinstance(boards, list) or not boards or len(set(boards)) != len(boards) or
            any(b not in ('j613', 'j615') for b in boards)):
        raise ValueError('legacy GPU boards must name supported Air boards once')
    if 'j615' in boards and sources['kernel'] != '2cd8be6fbaf8cd91e36197edf161dde44264e171':
        raise ValueError('J615 legacy GPU requires the matched J615 kernel consumer')
    if 'j615' in boards and sources['m1n1'] != '74ba6bea52d1f865d204bb3f8168705a148fd5c5':
        raise ValueError('J615 legacy GPU requires the matched J615 m1n1 producer')
    packages = manifest['packages']
    if set(packages) != set(ROLES): raise ValueError('manifest must name every matched and auxiliary package')
    pins = {}; resolved = {}
    for role, pkgname in ROLES.items():
        info = packages[role]; path = directory / info['file']
        if path.name != info['file'] or not re.fullmatch(r'[A-Za-z0-9._+:-]+\.pkg\.tar\.zst', path.name):
            raise ValueError('package filename must be a local basename')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if not HEX.fullmatch(info['sha256']) or digest != info['sha256']: raise ValueError(f'{role} artifact hash differs')
        metadata = member(path,'.PKGINFO').decode()
        if re.findall(r'^pkgname = (.+)$', metadata, re.M) != [pkgname]: raise ValueError(f'{role} package identity differs')
        if re.findall(r'^arch = (.+)$', metadata,re.M) not in (['aarch64'], ['any']): raise ValueError('unsupported package architecture')
        if role in ('kernel','headers') and re.findall(r'^pkgver = (.+)$',metadata,re.M) != [manifest['version']]:
            raise ValueError('kernel package version and installer version differ')
        pins[role] = f'{path.name} {digest}'; resolved[role] = path
    binary = member(resolved['m1n1'],'usr/lib/asahi-boot/m1n1.bin')
    binary_sha = hashlib.sha256(binary).hexdigest()
    if binary_sha != manifest.get('m1n1_bin_sha256'): raise ValueError('unified m1n1 binary hash differs')
    for marker in (b'apple,j613-25g83-mapping-handoff', b'apple,j613-25g83-gpu-handoff'):
        if marker not in binary: raise ValueError('unified m1n1 lacks required handoff')
    native = member(resolved['mesa'],'opt/mesa-m3/25g83/share/mesa-m3/profile')
    if native != b'j613-25g83-gl-only\n': raise ValueError('native Mesa marker differs')
    hook = member(resolved['mesa'],'usr/share/uwsm/env.d/50-mesa-m3')
    if not re.search(rb'(?m)^[ \t]*\.[ \t]+/opt/mesa-m3/libexec/mesa-m3-session-env[ \t]*$', hook):
        raise ValueError('Mesa hook session delegation differs')
    session = member(resolved['mesa'],'opt/mesa-m3/libexec/mesa-m3-session-env')
    if b'j613-25g83-hal200' not in session: raise ValueError('Mesa session selector differs')
    member(resolved['mesa'],'opt/mesa-m3/libexec/mesa-m3-abi-check')
    listing = subprocess.check_output(['bsdtar','-tf',str(resolved['kernel'])],text=True).splitlines()
    dt_path = re.compile(r'usr/lib/modules/[^/]+/dtbs/(?:apple/)?t8122-j613-25g83\.dtb')
    dtbs = [p for p in listing if dt_path.fullmatch(p)]
    if len(dtbs) != 1: raise ValueError('kernel must supply exactly one separate J61325 DTB')
    dt = member(resolved['kernel'],dtbs[0])
    for marker in (b'apple,j613-25g83-profile\0', b'apple,firmware-compat\0'):
        if marker not in dt: raise ValueError('J61325 DTB profile is missing')
    for field in ('stage1_25_versions',):
        versions = manifest.get(field,[])
        if not versions or any(not re.fullmatch(r'[A-Za-z0-9._+-]+',v) for v in versions):
            raise ValueError('qualified source-built stage1 versions required for 25 profile')
    stack_id = hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    s = template
    substitutions = dict(VERSION=manifest['version'],TAG=manifest['tag'],M1N1_BIN_SHA=binary_sha,
                         DESKTOP_FIXES_DATA=desktop_data(manifest, directory),
                         M3_STACK_ID=stack_id, M3_PERSISTENT_BOARDS=' '.join(boards), M3_STAGE1_25_VERSIONS=' '.join(manifest['stage1_25_versions']),
                         M1N1_PACKAGE=pins['m1n1'],M3_PRO_MESA_PACKAGE=pins['mesa'])
    for key,value in substitutions.items():
        replacement = key+'="'+value+'"' if key not in ('VERSION','TAG','M1N1_BIN_SHA') else key+'='+value
        s,n = re.subn(r'^'+key+r'=.*$',lambda _:replacement,s,count=1,flags=re.M)
        if n != 1: raise ValueError(f'template field {key} missing')
    entries = '\n'.join('  "'+pins[k]+'"' for k in ('kernel','headers','libfprint','touchid'))
    s,n = re.subn(r'^PACKAGES=\(\n.*?^\)',lambda _:'PACKAGES=(\n'+entries+'\n)',s,count=1,flags=re.M|re.S)
    if n != 1: raise ValueError('template package list missing')
    # Package detector/list/dependencies follow the actual unified Mesa package.
    metadata = member(resolved['mesa'],'.PKGINFO').decode()
    needs = ' '.join(re.findall(r'^depend = (.+)$',metadata,re.M))
    s = re.sub(r'^M3_PRO_MESA_NEEDS=.*$',lambda _:'M3_PRO_MESA_NEEDS="'+needs+'"',s,flags=re.M)
    for name,key in [('opt/mesa-m3/libexec/mesa-m3-user-setup','M3_PRO_MESA_DETECTOR_SHA256'),('opt/mesa-m3/share/mesa-m3/user-setup.list','M3_PRO_MESA_SETUP_LIST_SHA256')]:
        digest = hashlib.sha256(member(resolved['mesa'],name)).hexdigest()
        s = re.sub(r'^'+key+r'=.*$',lambda _:key+'='+digest,s,flags=re.M)
    return s, stack_id

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('manifest',type=Path); p.add_argument('artifacts',type=Path); p.add_argument('output',type=Path)
    p.add_argument('--template',type=Path,default=Path(__file__).with_name('install-aurora-sep.sh'))
    a=p.parse_args()
    try: script,stack = assemble(a.template.read_text(),json.loads(a.manifest.read_text()),a.artifacts)
    except (ValueError,OSError,KeyError,subprocess.CalledProcessError) as e: p.error(str(e))
    with a.output.open('x') as out: out.write(script)
    print(stack)

if __name__ == '__main__': main()
