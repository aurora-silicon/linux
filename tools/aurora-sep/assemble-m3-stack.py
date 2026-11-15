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
NEO_KERNEL = '77030de18bbbbf077777da5f821e9c15dd1e4c47'
NEO_KERNELS = (NEO_KERNEL, 'a6a62e586021d9f786d6a96b4ded6b0ad3b613fa',
               'e76133daffab1be549b47571691ab77ac7c28201',
               '25b138b77409fcb49c2e4fbebee57d81bdea9bb3',
               '417c8e5e26319366cb5fff32d9887abfaa599e76',
               '9b96a8f6a29ceb3f645ed75e220a71c10e7d8e1f',
               '962bc344dd5c3725b63754e72b9cace43ca8e17e',
               'bb07ce91aaded19baf92204ea930fc65cfd30aea',
               '9189760ca2222ba57d802d65ff3a0ebb2ca985ab',
               'a29fbe8f9244d3ab42fae7aa188c62e980d40a82',
               '0a7037c9871fd1bde2923c3c7f27732d53519ed2',
               'a85f8eee2fd54ed9e7237f25d1f085e9d7848111',
               '7cae5a72ccc6690a710e199c853f8e1c97a3d86c',
               'f240960cc84cfda6f73e188050ccadb9435e938f',
               'd49021440d8ca02bbc8ecc17c19ce76c048b6236',
               '6f63677751854d57bcfac172314957bc2197c05f',
               '5ec1028c81526e1ed31db2b97e47edf8663e83cb')
# J615 native25 requires exact kernel, boot source and packaged binary bindings.
J615_NATIVE25_PAIR = {
    'kernel': '9189760ca2222ba57d802d65ff3a0ebb2ca985ab',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_PREVIOUS_NATIVE25_PAIR = {
    'kernel': 'bb07ce91aaded19baf92204ea930fc65cfd30aea',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_TUNNEL_NATIVE25_PAIR = {
    'kernel': 'a29fbe8f9244d3ab42fae7aa188c62e980d40a82',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_NIC_NATIVE25_PAIR = {
    'kernel': '0a7037c9871fd1bde2923c3c7f27732d53519ed2',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_DISPLAY_READY_NATIVE25_PAIR = {
    'kernel': 'a85f8eee2fd54ed9e7237f25d1f085e9d7848111',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_RELEASE14_NATIVE25_PAIR = {
    'kernel': '7cae5a72ccc6690a710e199c853f8e1c97a3d86c',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_RELEASE15_NATIVE25_PAIR = {
    'kernel': 'f240960cc84cfda6f73e188050ccadb9435e938f',
    'm1n1': 'd05ba17dfb771871d608225be42968bbb1e75a18',
    'm1n1_bin_sha256': '396cf3ec1774ca9d15ca1b6f71b5e8a3639b27f80b95d530722b35a5ea35141d',
}
J615_RELEASE16_NATIVE25_PAIR = dict(J615_RELEASE15_NATIVE25_PAIR, kernel='d49021440d8ca02bbc8ecc17c19ce76c048b6236')
J615_RELEASE18_NATIVE25_PAIR = dict(J615_RELEASE15_NATIVE25_PAIR, kernel='6f63677751854d57bcfac172314957bc2197c05f')
J615_RELEASE19_NATIVE25_PAIR = dict(J615_RELEASE15_NATIVE25_PAIR, kernel='5ec1028c81526e1ed31db2b97e47edf8663e83cb')
# Reserved firmware DART tables require the matched Neo ESP producer.
NEO_PREVIOUS_FW_ROOT_PAIR = {
    'kernel': '9b96a8f6a29ceb3f645ed75e220a71c10e7d8e1f',
    'm1n1': 'a67a0420804e5096715d95b5f0fff4eed181557e',
    'm1n1_bin_sha256': '3eaa76ca39454b4aa232efecba72ffb3770b30a3b1047320c8f50c8ef627056c',
}
NEO_FW_ROOT_PAIR = {
    'kernel': '9189760ca2222ba57d802d65ff3a0ebb2ca985ab',
    'm1n1': '8b6cba90b4acc9c5817ae050a034dbef41177f24',
    'm1n1_bin_sha256': 'bfe606c41c3b92bc95ea7bba384ac8d8de0e6d0a38a878e5f2de79488519f1b1',
}
NEO_PREVIOUS_RELEASE_FW_ROOT_PAIR = {
    'kernel': 'bb07ce91aaded19baf92204ea930fc65cfd30aea',
    'm1n1': '8b6cba90b4acc9c5817ae050a034dbef41177f24',
    'm1n1_bin_sha256': 'bfe606c41c3b92bc95ea7bba384ac8d8de0e6d0a38a878e5f2de79488519f1b1',
}
NEO_RELEASED_FW_ROOT_PAIR = {
    'kernel': '962bc344dd5c3725b63754e72b9cace43ca8e17e',
    'm1n1': '8b6cba90b4acc9c5817ae050a034dbef41177f24',
    'm1n1_bin_sha256': 'bfe606c41c3b92bc95ea7bba384ac8d8de0e6d0a38a878e5f2de79488519f1b1',
}
# A live H17P session uses the same reserved-table kernel consumer.
NEO_LIVE_FW_ROOT_PAIR = {
    'kernel': '9189760ca2222ba57d802d65ff3a0ebb2ca985ab',
    'm1n1': '1b36287e04788f4a028e6180c64d3d96e49f06b1',
    'm1n1_bin_sha256': '1c61eea4da8bd4bd2e18de534d661ccc569e1b8e92b19d4af867d5b281516491',
}
NEO_TUNNEL_LIVE_FW_ROOT_PAIR = {
    'kernel': 'a29fbe8f9244d3ab42fae7aa188c62e980d40a82',
    'm1n1': '1b36287e04788f4a028e6180c64d3d96e49f06b1',
    'm1n1_bin_sha256': '1c61eea4da8bd4bd2e18de534d661ccc569e1b8e92b19d4af867d5b281516491',
}
NEO_NIC_LIVE_FW_ROOT_PAIR = {
    'kernel': '0a7037c9871fd1bde2923c3c7f27732d53519ed2',
    'm1n1': '1b36287e04788f4a028e6180c64d3d96e49f06b1',
    'm1n1_bin_sha256': '1c61eea4da8bd4bd2e18de534d661ccc569e1b8e92b19d4af867d5b281516491',
}
NEO_DISPLAY_READY_LIVE_FW_ROOT_PAIR = {
    'kernel': 'a85f8eee2fd54ed9e7237f25d1f085e9d7848111',
    'm1n1': '1b36287e04788f4a028e6180c64d3d96e49f06b1',
    'm1n1_bin_sha256': '1c61eea4da8bd4bd2e18de534d661ccc569e1b8e92b19d4af867d5b281516491',
}
NEO_RELEASE14_LIVE_FW_ROOT_PAIR = {
    'kernel': '7cae5a72ccc6690a710e199c853f8e1c97a3d86c',
    'm1n1': '1b36287e04788f4a028e6180c64d3d96e49f06b1',
    'm1n1_bin_sha256': '1c61eea4da8bd4bd2e18de534d661ccc569e1b8e92b19d4af867d5b281516491',
}
NEO_RELEASE15_LIVE_FW_ROOT_PAIR = {
    'kernel': 'f240960cc84cfda6f73e188050ccadb9435e938f',
    'm1n1': '1b36287e04788f4a028e6180c64d3d96e49f06b1',
    'm1n1_bin_sha256': '1c61eea4da8bd4bd2e18de534d661ccc569e1b8e92b19d4af867d5b281516491',
}
NEO_RELEASE16_LIVE_FW_ROOT_PAIR = dict(NEO_RELEASE15_LIVE_FW_ROOT_PAIR, kernel='d49021440d8ca02bbc8ecc17c19ce76c048b6236')
NEO_RELEASE18_LIVE_FW_ROOT_PAIR = dict(NEO_RELEASE15_LIVE_FW_ROOT_PAIR, kernel='6f63677751854d57bcfac172314957bc2197c05f')
NEO_RELEASE19_LIVE_FW_ROOT_PAIR = dict(NEO_RELEASE15_LIVE_FW_ROOT_PAIR, kernel='5ec1028c81526e1ed31db2b97e47edf8663e83cb')
STANDARD_M1N1_SOURCE = "d05ba17dfb771871d608225be42968bbb1e75a18"
PREVIOUS_STANDARD_M1N1_SOURCE = "31501778e863feb5d2afe77be3a1edc588c98119"
J615_LEGACY_M1N1_SOURCES = ("74ba6bea52d1f865d204bb3f8168705a148fd5c5",
                          PREVIOUS_STANDARD_M1N1_SOURCE, STANDARD_M1N1_SOURCE)

def member(path, name):
    return subprocess.check_output(['bsdtar', '-xOf', str(path), name])

def check_kernel_bootloader_dependency(metadata):
    # Kernel-only installs retain m1n1; GPU handoffs have separate admission gates.
    dependencies = re.findall(r'^depend = (.+)$', metadata, re.M)
    bootloader = [d for d in dependencies if re.match(r'^m1n1(?:$|[<>=])', d)]
    if bootloader != ['m1n1>=1.6.1']:
        raise ValueError('kernel must require m1n1>=1.6.1; newer bootloaders belong to matched GPU profiles')
    if any(re.match(r'^m1n1-(?:aurora|neo)(?:$|[<>=])', d) for d in dependencies):
        raise ValueError('kernel-only installs must not require a GPU-profile bootloader package')

def optional_member(path, name):
    result = subprocess.run(['bsdtar', '-xOf', str(path), name], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return result.stdout if result.returncode == 0 else None

def check_standard_m1n1_source(source, package):
    if source not in (STANDARD_M1N1_SOURCE, PREVIOUS_STANDARD_M1N1_SOURCE):
        return
    if optional_member(package, 'usr/share/m1n1-aurora/source') != (source + '\n').encode():
        raise ValueError('standard m1n1 source marker differs')
    expected = b'release=1\nchainloading=0\nj613_esp_stage1=0\ntag=v1.6.1-omarchy.aurora17\n'
    if optional_member(package, 'usr/share/m1n1-aurora/build-config') != expected:
        raise ValueError('standard m1n1 build configuration differs')

def check_j615_native25(sources, binary_sha, mesa):
    pair = J615_NATIVE25_PAIR
    if sources['kernel'] == J615_PREVIOUS_NATIVE25_PAIR['kernel']:
        pair = J615_PREVIOUS_NATIVE25_PAIR
    if sources['kernel'] == J615_TUNNEL_NATIVE25_PAIR['kernel']:
        pair = J615_TUNNEL_NATIVE25_PAIR
    if sources['kernel'] == J615_NIC_NATIVE25_PAIR['kernel']:
        pair = J615_NIC_NATIVE25_PAIR
    if sources['kernel'] == J615_DISPLAY_READY_NATIVE25_PAIR['kernel']:
        pair = J615_DISPLAY_READY_NATIVE25_PAIR
    if sources['kernel'] == J615_RELEASE14_NATIVE25_PAIR['kernel']:
        pair = J615_RELEASE14_NATIVE25_PAIR
    if sources['kernel'] == J615_RELEASE15_NATIVE25_PAIR['kernel']:
        pair = J615_RELEASE15_NATIVE25_PAIR
    if sources['kernel'] == J615_RELEASE16_NATIVE25_PAIR['kernel']:
        pair = J615_RELEASE16_NATIVE25_PAIR
    if sources['kernel'] == J615_RELEASE18_NATIVE25_PAIR['kernel']:
        pair = J615_RELEASE18_NATIVE25_PAIR
    if sources['kernel'] == J615_RELEASE19_NATIVE25_PAIR['kernel']:
        pair = J615_RELEASE19_NATIVE25_PAIR
    if not isinstance(pair, dict) or set(pair) != {'kernel', 'm1n1', 'm1n1_bin_sha256'}:
        raise ValueError('J615 native25 requires the recorded J615 boot/kernel pair (J615_NATIVE25_PAIR)')
    if sources['kernel'] != pair['kernel']: raise ValueError('J615 native25 requires the matched J615 kernel')
    if sources['m1n1'] != pair['m1n1']: raise ValueError('J615 native25 requires the matched J615 m1n1 source')
    if binary_sha != pair['m1n1_bin_sha256']: raise ValueError('J615 native25 requires the matched J615 m1n1 binary')
    boards = optional_member(mesa, 'opt/mesa-m3/share/mesa-m3/native25-boards')
    if boards is None or b'j615-experimental' not in boards.splitlines() or b'j613' not in boards.splitlines():
        raise ValueError('Mesa does not declare the J615 25G83 session capability')
    if b'asahi,j615-25g83-experimental' not in member(mesa, 'opt/mesa-m3/libexec/mesa-m3-session-env'):
        raise ValueError('Mesa session hook lacks the J615 25G83 admission')

def check_neo_firmware_tables(kernel, sources, binary_sha):
    pairs = [p for p in (NEO_FW_ROOT_PAIR, NEO_PREVIOUS_RELEASE_FW_ROOT_PAIR,
                        NEO_RELEASED_FW_ROOT_PAIR, NEO_PREVIOUS_FW_ROOT_PAIR,
                        NEO_LIVE_FW_ROOT_PAIR, NEO_TUNNEL_LIVE_FW_ROOT_PAIR,
                        NEO_NIC_LIVE_FW_ROOT_PAIR, NEO_DISPLAY_READY_LIVE_FW_ROOT_PAIR,
                        NEO_RELEASE14_LIVE_FW_ROOT_PAIR,
                        NEO_RELEASE15_LIVE_FW_ROOT_PAIR, NEO_RELEASE16_LIVE_FW_ROOT_PAIR,
                        NEO_RELEASE18_LIVE_FW_ROOT_PAIR, NEO_RELEASE19_LIVE_FW_ROOT_PAIR) if kernel == p['kernel']]
    if not pairs:
        return
    if any(not re.fullmatch(r'[0-9a-f]{40}', p.get('m1n1') or '') for p in pairs):
        raise ValueError('Neo firmware tables require a recorded boot source')
    pair = next((p for p in pairs if sources['m1n1'] == p['m1n1']), None)
    if pair is None:
        raise ValueError('Neo firmware tables require the matched boot source')
    if binary_sha != pair['m1n1_bin_sha256']:
        raise ValueError('Neo firmware tables require the matched boot binary')


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

def neo_data(manifest, directory):
    neo = manifest.get('neo')
    if neo is None:
        return {}
    if manifest['source_commits']['kernel'] not in NEO_KERNELS:
        raise ValueError('Neo requires the matched T8140 kernel')
    if neo.get('profile') != 'j700-g17p-hal200':
        raise ValueError('Neo profile differs')
    sources = neo.get('source_commits', {})
    if any(not re.fullmatch(r'[0-9a-f]{40}', sources.get(k, '')) for k in ('mesa', 'm1n1')):
        raise ValueError('exact Neo Mesa and m1n1 source commits are required')
    packages = neo.get('packages', {})
    if set(packages) != {'mesa', 'm1n1'}:
        raise ValueError('Neo needs both matched packages')
    pins = {}; paths = {}
    for role, name in (('mesa', 'mesa-neo'), ('m1n1', 'm1n1-neo')):
        item = packages[role]; path = directory / item['file']
        if path.name != item['file'] or not re.fullmatch(r'[A-Za-z0-9._+:-]+\.pkg\.tar\.zst', path.name):
            raise ValueError('Neo package filename must be a local basename')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if not HEX.fullmatch(item['sha256']) or digest != item['sha256']:
            raise ValueError('Neo package hash differs')
        info = member(path, '.PKGINFO').decode()
        if re.findall(r'^pkgname = (.+)$', info, re.M) != [name] or re.findall(r'^arch = (.+)$', info, re.M) != ['aarch64']:
            raise ValueError('Neo package identity differs')
        pins['NEO_'+role.upper()+'_PACKAGE'] = path.name+' '+digest
        paths[role] = path
    boot = member(paths['m1n1'], 'usr/lib/m1n1-neo/m1n1.bin')
    digest = hashlib.sha256(boot).hexdigest()
    if digest != neo.get('m1n1_bin_sha256'):
        raise ValueError('Neo bootloader hash differs')
    check_neo_firmware_tables(manifest['source_commits']['kernel'], sources, digest)
    pins['NEO_M1N1_BIN_SHA'] = digest
    if member(paths['m1n1'], 'usr/share/m1n1-neo/source').strip().decode() != sources['m1n1']:
        raise ValueError('Neo bootloader source differs')
    cfg = member(paths['m1n1'], 'usr/share/m1n1-neo/build-config.h')
    for flag in (b'RELEASE', b'CHAINLOADING'):
        if not re.search(rb'^#define '+flag+rb'$', cfg, re.M):
            raise ValueError('Neo ESP configuration differs')
    if not re.search(rb'^#define (?:ESP_STAGE2|J700_ESP_STAGE2)$', cfg, re.M):
        raise ValueError('Neo ESP configuration differs')
    if any(b'#define '+flag in cfg for flag in (b'T8140_KIS_PROXY', b'J700_CDC_PROXY', b'J613_ESP_STAGE1')):
        raise ValueError('Neo ESP must not use a debug proxy')
    for role, path in (('mesa', 'opt/mesa-neo/share/mesa-neo/profile'), ('m1n1', 'usr/share/m1n1-neo/profile')):
        if member(paths[role], path) != b'j700-g17p-hal200\n':
            raise ValueError('Neo installed profile marker differs')
    capabilities = member(paths['mesa'], 'opt/mesa-neo/share/mesa-neo/capabilities')
    if capabilities != b'opengl=native-experimental\nvulkan=honeykrisp-experimental\n':
        raise ValueError('Neo graphics capabilities differ')
    for path in ('libexec/mesa-neo-abi-check', 'libexec/mesa-neo-loadcheck', 'bin/mesa-neo-probe',
                 'share/vulkan/icd.d/asahi_icd.aarch64.json'):
        member(paths['mesa'], 'opt/mesa-neo/'+path)
    hook = member(paths['mesa'], 'usr/share/uwsm/env.d/51-mesa-neo')
    if not re.search(rb'(?m)^ *\. /opt/mesa-neo/libexec/mesa-neo-session-env$', hook):
        raise ValueError('Neo session delegation differs')
    for installed, local in (('libexec/mesa-neo-env', 'mesa-neo-env'),
                             ('libexec/mesa-neo-session-env', 'mesa-neo-session-env'),
                             ('bin/mesa-neo-probe', 'mesa-neo-probe-wrapper')):
        canonical = Path(__file__).with_name('neo') / local
        if member(paths['mesa'], 'opt/mesa-neo/' + installed) != canonical.read_bytes():
            raise ValueError('Neo package helper differs from installer: ' + installed)
    member(paths['mesa'], 'opt/mesa-neo/libexec/mesa-neo-probe.real')
    return pins

def pro_data(manifest, directory):
    pro = manifest.get('pro')
    if pro is None:
        return {}
    if pro.get('profile') != 't6030-g15s-legacy-hal':
        raise ValueError('Pro performance profile differs')
    if manifest['source_commits']['kernel'] != '5ec1028c81526e1ed31db2b97e47edf8663e83cb':
        raise ValueError('Pro performance kernel differs')
    expected = {'native': ('mesa-m3-native-pro', 'opt/mesa-m3/native-pro', 'd616d0650d6fa80f72a750a0121442ab917ed6cf', b't6030-native-gl-legacy-hal\n'),
                'vulkan': ('mesa-m3-pro-vulkan', 'opt/mesa-m3/pro-vulkan', 'b0f56952b7f06955b143fbfa90c07de4c5864964', b't6030-pro-vulkan-legacy-hal\n')}
    if set(pro.get('packages', {})) != set(expected):
        raise ValueError('Pro package pair is incomplete')
    kernel_path = directory / manifest['packages']['kernel']['file']
    listing = subprocess.check_output(['bsdtar', '-tf', str(kernel_path)], text=True).splitlines()
    markers = [name for name in listing if re.fullmatch(r'usr/lib/modules/[^/]+/aurora-m3pro-profile', name)]
    if len(markers) != 1 or member(kernel_path, markers[0]) != b't6030-perf-v1\nsource=5ec1028c81526e1ed31db2b97e47edf8663e83cb\n':
        raise ValueError('Pro kernel performance marker differs')
    pins = {}
    for role, (name, prefix, source, marker) in expected.items():
        item = pro['packages'][role]
        path = directory / item['file']
        if path.name != item['file'] or not re.fullmatch(r'[A-Za-z0-9._+:-]+\.pkg\.tar\.zst', path.name):
            raise ValueError('Pro package filename differs')
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise ValueError('Pro package hash differs')
        if re.findall(r'^pkgname = (.+)$', member(path, '.PKGINFO').decode(), re.M) != [name]:
            raise ValueError('Pro package identity differs')
        if re.findall(r'^arch = (.+)$', member(path, '.PKGINFO').decode(), re.M) != ['aarch64']:
            raise ValueError('Pro package architecture differs')
        if pro['source_commits'].get(role) != source:
            raise ValueError('Pro Mesa source differs')
        if member(path, prefix + '/share/mesa-m3/profile') != marker:
            raise ValueError('Pro installed marker differs')
        if ('source=' + source).encode() not in member(path, prefix + '/share/mesa-m3/capabilities').splitlines():
            raise ValueError('Pro packaged source differs')
        if role == 'native':
            for file in ('mesa-m3-pro-check', 'mesa-m3-pro-performance', 'mesa-m3-pro-session-env'):
                if member(path, prefix + '/libexec/' + file) != Path(__file__).with_name('pro').joinpath(file).read_bytes():
                    raise ValueError('Pro runtime helper differs')
            if member(path, 'usr/share/uwsm/env.d/51-mesa-m3-pro') != Path(__file__).with_name('pro').joinpath('51-mesa-m3-pro').read_bytes():
                raise ValueError('Pro session hook differs')
            for installed, local in [('usr/lib/systemd/system/mesa-m3-pro-performance.service', 'mesa-m3-pro-performance.service'), (prefix + '/share/drirc.d/10-m3-pro.conf', '10-m3-pro.conf')]:
                if member(path, installed) != Path(__file__).with_name('pro').joinpath(local).read_bytes():
                    raise ValueError('Pro session integration differs')
        key = 'M3_PRO_NATIVE_PACKAGE' if role == 'native' else 'M3_PRO_VULKAN_PACKAGE'
        pins[key] = path.name + ' ' + item['sha256']
    pins['M3_PRO_PERF_MIGRATOR'] = base64.b64encode(Path(__file__).with_name('pro').joinpath('migrate-session.py').read_bytes()).decode()
    return pins

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
    if 'j615' in boards and sources['kernel'] not in ('a4d7ff4acdefcbce7daa7f57866413f21f05fb75', *NEO_KERNELS):
        raise ValueError('J615 legacy GPU requires the matched J615 kernel consumer')
    if 'j615' in boards and sources['m1n1'] not in J615_LEGACY_M1N1_SOURCES:
        raise ValueError('J615 legacy GPU requires the matched J615 m1n1 producer')
    # 25G83 is one firmware ABI profile (j613-25g83); J615 opts in to it as an experiment.
    native25 = manifest.get('native25_boards', ['j613'])
    if (not isinstance(native25, list) or 'j613' not in native25 or len(set(native25)) != len(native25) or
            any(b not in ('j613', 'j615') for b in native25)):
        raise ValueError('native25 boards must name supported Air boards once, including j613')
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
        if role == 'kernel': check_kernel_bootloader_dependency(metadata)
        pins[role] = f'{path.name} {digest}'; resolved[role] = path
    check_standard_m1n1_source(sources['m1n1'], resolved['m1n1'])
    binary = member(resolved['m1n1'],'usr/lib/asahi-boot/m1n1.bin')
    binary_sha = hashlib.sha256(binary).hexdigest()
    if binary_sha != manifest.get('m1n1_bin_sha256'): raise ValueError('unified m1n1 binary hash differs')
    for marker in (b'apple,j613-25g83-mapping-handoff', b'apple,j613-25g83-gpu-handoff'):
        if marker not in binary: raise ValueError('unified m1n1 lacks required handoff')
    if 'j615' in native25 and b'asahi,j615-25g83-experimental' not in binary:
        raise ValueError('unified m1n1 lacks the experimental J615 25G83 switch')
    native = member(resolved['mesa'],'opt/mesa-m3/25g83/share/mesa-m3/profile')
    if native != b'j613-25g83-gl-only\n': raise ValueError('native Mesa marker differs')
    hook = member(resolved['mesa'],'usr/share/uwsm/env.d/50-mesa-m3')
    if not re.search(rb'(?m)^[ \t]*\.[ \t]+/opt/mesa-m3/libexec/mesa-m3-session-env[ \t]*$', hook):
        raise ValueError('Mesa hook session delegation differs')
    session = member(resolved['mesa'],'opt/mesa-m3/libexec/mesa-m3-session-env')
    if b'j613-25g83-hal200' not in session: raise ValueError('Mesa session selector differs')
    member(resolved['mesa'],'opt/mesa-m3/libexec/mesa-m3-abi-check')
    if 'j615' in native25: check_j615_native25(sources, binary_sha, resolved['mesa'])
    listing = subprocess.check_output(['bsdtar','-tf',str(resolved['kernel'])],text=True).splitlines()
    dt_path = re.compile(r'usr/lib/modules/[^/]+/dtbs/(?:apple/)?t8122-j613-25g83\.dtb')
    dtbs = [p for p in listing if dt_path.fullmatch(p)]
    if len(dtbs) != 1: raise ValueError('kernel must supply exactly one separate J61325 DTB')
    dt = member(resolved['kernel'],dtbs[0])
    for marker in (b'apple,j613-25g83-profile\0', b'apple,firmware-compat\0'):
        if marker not in dt: raise ValueError('J61325 DTB profile is missing')
    if 'j615' in native25:
        dt_path = re.compile(r'usr/lib/modules/[^/]+/dtbs/(?:apple/)?t8122-j615-25g83\.dtb')
        dtbs = [p for p in listing if dt_path.fullmatch(p)]
        if len(dtbs) != 1: raise ValueError('kernel must supply exactly one separate J61525 DTB')
        dt = member(resolved['kernel'],dtbs[0])
        for marker in (b'apple,j613-25g83-profile\0', b'apple,firmware-compat\0', b'apple,j615\0'):
            if marker not in dt: raise ValueError('J61525 DTB profile is missing')
    fields = ('stage1_25_versions','stage1_25_j615_versions') if 'j615' in native25 else ('stage1_25_versions',)
    for field in fields:
        versions = manifest.get(field,[])
        if (not isinstance(versions, list) or not versions or
                any(not isinstance(v, str) or not re.fullmatch(r'[A-Za-z0-9._+-]+',v) for v in versions)):
            raise ValueError(f'qualified source-built stage1 versions required for 25 profile ({field})')
    neo_pins = neo_data(manifest, directory)
    stack_id = hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    s = template
    substitutions = dict(VERSION=manifest['version'],TAG=manifest['tag'],M1N1_BIN_SHA=binary_sha,
                         DESKTOP_FIXES_DATA=desktop_data(manifest, directory),
                         M3_STACK_ID=stack_id, M3_PERSISTENT_BOARDS=' '.join(boards), M3_STAGE1_25_VERSIONS=' '.join(manifest['stage1_25_versions']),
                         M3_NATIVE25_BOARDS=' '.join(native25),
                         M3_STAGE1_25_J615_VERSIONS=' '.join(manifest['stage1_25_j615_versions']) if 'j615' in native25 else '',
                         M1N1_PACKAGE=pins['m1n1'],M3_PRO_MESA_PACKAGE=pins['mesa'])
    substitutions.update(neo_pins)
    substitutions.update(pro_data(manifest, directory))
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
