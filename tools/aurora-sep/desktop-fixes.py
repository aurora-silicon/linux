#!/usr/bin/env python3
"""Check optional desktop packages without changing the installed system."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile

NAMES = {'omarchy': '4.0.4-2', 'omarchy-settings': '4.0.4-2',
         'aquamarine': '0.15.1-1.3'}
PAM_LINE = 'session required pam_exec.so seteuid /usr/bin/omarchy-session-guard --pam'
HEX = re.compile(r'[0-9a-f]{64}')


def command(*args, accepted=(0,)):
    result = subprocess.run(args, text=True, capture_output=True,
                            env={**os.environ, 'LC_ALL': 'C'})
    if result.returncode not in accepted:
        raise ValueError(f'{args[0]} failed: {result.stderr.strip()}')
    if result.returncode == 127 and not result.stdout.strip():
        raise ValueError(f'{args[0]} dependency query failed without a result')
    return result.stdout


def safe_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or str(path) != name or not path.parts:
        raise ValueError('invalid desktop payload path')
    return name


def immutable(name):
    return name.startswith('usr/bin/') or name.startswith('usr/share/omarchy/')


def validate_manifest(data):
    if data.get('schema') != 'aurora.desktop-fixes/1' or set(data.get('packages', {})) != set(NAMES):
        raise ValueError('desktop manifest must name exactly the matched three packages')
    for name, version in NAMES.items():
        item = data['packages'][name]
        if item.get('version') != version or not HEX.fullmatch(item.get('sha256', '')):
            raise ValueError('desktop package pin differs')
        if not re.fullmatch(r'[A-Za-z0-9._+:-]+\.pkg\.tar\.zst', item.get('file', '')):
            raise ValueError('desktop artifact must be a local basename')
        for path, digest in item.get('payload', {}).items():
            safe_path(path)
            if not HEX.fullmatch(digest):
                raise ValueError('desktop payload hash differs')
    catalogs = data.get('omarchy_catalogs', {})
    if set(catalogs) != {'4.0.4-1', '4.0.4-2'}:
        raise ValueError('both stable runtime catalogs are required')
    for catalog in catalogs.values():
        if not catalog.get('files') or not re.fullmatch(r'[0-9a-f]{40}', catalog.get('source', '')):
            raise ValueError('exact runtime source catalog is required')
        for path, item in catalog['files'].items():
            safe_path(path)
            if not immutable(path):
                raise ValueError('runtime catalog contains another file scope')
            if set(item) == {'sha256', 'mode'}:
                if not HEX.fullmatch(item['sha256']) or item['mode'] not in (0o644, 0o755):
                    raise ValueError('invalid runtime digest')
            elif set(item) != {'symlink'} or not isinstance(item['symlink'], str):
                raise ValueError('invalid runtime catalog entry')
    required = {
        'omarchy': ('usr/bin/omarchy-session-guard', 'usr/share/omarchy/shell/session-guard.py',
                    'usr/share/omarchy/shell/session-guard-install.py',
                    'usr/share/omarchy/shell/plugins/lock/Service.qml',
                    'usr/share/libalpm/hooks/95-omarchy-session-guard.hook'),
        'omarchy-settings': ('usr/lib/systemd/user/wayland-wm@hyprland.desktop.service.d/99-session-lock-recovery.conf',
                             'usr/local/share/wayland-sessions/omarchy.desktop',
                             'usr/local/share/wayland-sessions/omarchy-guarded-hyprland.desktop',
                             'etc/sddm.conf.d/90-session-lock-recovery.conf')}
    for name, paths in required.items():
        if not set(paths) <= set(data['packages'][name].get('payload', {})):
            raise ValueError('desktop guard, service and recovery pins are required')
    activation = {p for name in required for p in data['packages'][name]['payload'] if not immutable(p)}
    for catalog in catalogs.values():
        if set(catalog.get('activation', {})) != activation:
            raise ValueError('exact installed launcher and recovery catalog is required')
        for path, digest in catalog['activation'].items():
            safe_path(path)
            if digest is not None and not HEX.fullmatch(digest):
                raise ValueError('invalid installed launcher digest')


def checked_file(root, name):
    path = root / safe_path(name)
    owner = 0 if root == Path('/') else os.getuid()
    for parent in reversed(path.parents):
        if parent == root.parent:
            continue
        if parent == root or root in parent.parents:
            info = parent.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022:
                raise ValueError('unsafe desktop parent: ' + str(parent))
    info = path.lstat()
    if info.st_uid != owner or (not stat.S_ISLNK(info.st_mode) and info.st_mode & 0o022):
        raise ValueError('unsafe desktop file: ' + name)
    return path, info


def verify_runtime(root, catalog):
    owned = command('pacman', '-Qql', 'omarchy', 'omarchy-settings').splitlines()
    listed = set()
    for path in owned:
        name = path.lstrip('/')
        if immutable(name) and not path.endswith('/'):
            listed.add(safe_path(name))
    if listed != set(catalog['files']):
        raise ValueError('installed immutable Omarchy file list differs from stable source')
    physical = set()
    tree = root / 'usr/share/omarchy'
    for parent, directories, files in os.walk(tree, followlinks=False):
        for leaf in files + [d for d in directories if (Path(parent) / d).is_symlink()]:
            physical.add(str((Path(parent) / leaf).relative_to(root)))
    if physical != {p for p in listed if p.startswith('usr/share/omarchy/')}:
        raise ValueError('unmapped files exist in the immutable Omarchy runtime tree')
    for name, item in catalog['files'].items():
        path, info = checked_file(root, name)
        if 'symlink' in item:
            if not stat.S_ISLNK(info.st_mode) or os.readlink(path) != item['symlink']:
                raise ValueError('Omarchy symlink differs: ' + name)
        elif (not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != item['mode'] or
              hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']):
            raise ValueError('Omarchy runtime source differs: ' + name)
    for name, digest in catalog['activation'].items():
        path = root / name
        owner = 0 if root == Path('/') else os.getuid()
        for parent in reversed(path.parents):
            if parent == root or root in parent.parents:
                if not parent.exists() and not parent.is_symlink():
                    continue
                info = parent.lstat()
                if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022:
                    raise ValueError('unsafe desktop activation parent: ' + str(parent))
        if digest is None:
            if path.exists() or path.is_symlink():
                raise ValueError('unexpected installed launcher or recovery file: ' + name)
        else:
            file, info = checked_file(root, name)
            if not stat.S_ISREG(info.st_mode) or hashlib.sha256(file.read_bytes()).hexdigest() != digest:
                raise ValueError('installed launcher or recovery source differs: ' + name)


def verify_recovery(root, current):
    pam, info = checked_file(root, 'etc/pam.d/sddm')
    if not stat.S_ISREG(info.st_mode):
        raise ValueError('SDDM PAM must be a regular file')
    if current and PAM_LINE not in pam.read_text().splitlines():
        raise ValueError('installed authenticated recovery hook is missing')
    effective = None
    paths = sorted((root / 'usr/lib/sddm/sddm.conf.d').glob('*.conf'))
    paths += sorted(set((root / 'etc/sddm.conf.d').glob('*.conf')) |
                    {root / 'etc/sddm.conf.d/90-session-lock-recovery.conf'})
    paths += [root / 'etc/sddm.conf']
    for path in paths:
        if path == root / 'etc/sddm.conf.d/90-session-lock-recovery.conf' and not current:
            if path.exists() or path.is_symlink():
                _, mode = checked_file(root, str(path.relative_to(root)))
                if not stat.S_ISREG(mode.st_mode):
                    raise ValueError('SDDM recovery config must be a regular file')
            text = '[Autologin]\nRelogin=false\n'
        elif path.exists() or path.is_symlink():
            checked, mode = checked_file(root, str(path.relative_to(root)))
            if not stat.S_ISREG(mode.st_mode):
                raise ValueError('SDDM config must be a regular file')
            text = checked.read_text()
        else:
            continue
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read_string(text)
        if config.has_option('Autologin', 'Relogin'):
            effective = config.get('Autologin', 'Relogin').strip().lower()
    if effective != 'false':
        raise ValueError('SDDM configuration overrides authenticated recovery Relogin=false')


def plan(data, root):
    installed = {}
    for line in command('pacman', '-Q').splitlines():
        name, version = line.split()
        installed[name] = version
    selected = []
    if 'omarchy-dev' in installed or 'omarchy-settings-dev' in installed:
        raise ValueError('development Omarchy packages are not a stable upgrade baseline')
    pair = [installed.get(name) for name in ('omarchy', 'omarchy-settings')]
    if any(pair):
        if pair[0] != pair[1] or pair[0] not in ('4.0.4-1', '4.0.4-2'):
            raise ValueError('Omarchy requires the exact matching stable 4.0.4 pair; newer or unknown sources are preserved')
        verify_runtime(root, data['omarchy_catalogs'][pair[0]])
        verify_recovery(root, pair[0] == '4.0.4-2')
        if pair[0] == '4.0.4-1':
            selected += ['omarchy', 'omarchy-settings']
        else:
            for name in ('omarchy', 'omarchy-settings'):
                for path, digest in data['packages'][name]['payload'].items():
                    file, info = checked_file(root, path)
                    if not stat.S_ISREG(info.st_mode) or hashlib.sha256(file.read_bytes()).hexdigest() != digest:
                        raise ValueError('installed guarded desktop payload differs: ' + path)
            guard, _ = checked_file(root, 'usr/bin/omarchy-session-guard')
            if not os.access(guard, os.X_OK):
                raise ValueError('installed session guard is not executable')
            print('Desktop fixes: exact guarded Omarchy pair verified; no reinstall.', file=sys.stderr)
    else:
        print('Desktop fixes: Omarchy absent; desktop settings are unchanged.', file=sys.stderr)
    version = installed.get('aquamarine')
    if version in ('0.15.1-1', '0.15.1-1.1', '0.15.1-1.2'):
        metadata = command('pacman', '-Qi', 'aquamarine')
        if re.search(r'(?m)^Provides\s*:\s*.*\blibaquamarine\.so=14-64\b', metadata):
            selected.append('aquamarine')
        else:
            print('Desktop fixes: Aquamarine ABI14 is not provided; keeping the installed library.', file=sys.stderr)
    else:
        print('Desktop fixes: Aquamarine absent, already fixed or outside the supported baseline; keeping it.', file=sys.stderr)
    return selected


def archive_metadata(path):
    values = {}
    for line in command('bsdtar', '-xOf', str(path), '.PKGINFO').splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            values.setdefault(key, []).append(value)
    return values


def package_contents(path):
    entries = {}
    process = subprocess.Popen(['bsdtar', '-cf', '-', '@' + str(path)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
            for member in archive:
                name = member.name.removeprefix('./')
                if member.isdir():
                    continue
                safe_path(name)
                if name in entries:
                    raise ValueError('duplicate desktop archive entry: ' + name)
                if member.isfile():
                    entries[name] = {'sha256': hashlib.file_digest(archive.extractfile(member), 'sha256').hexdigest(),
                                     'mode': member.mode & 0o777}
                elif member.issym():
                    entries[name] = {'symlink': member.linkname}
                else:
                    raise ValueError('unsupported desktop archive entry: ' + name)
        if process.wait() != 0:
            raise ValueError('desktop archive listing failed')
        return entries
    finally:
        process.stdout.close()
        process.stderr.close()
        if process.poll() is None:
            process.kill()
            process.wait()


def verify_archive(name, item, directory):
    path = directory / item['file']
    if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
        raise ValueError('desktop artifact checksum differs: ' + name)
    metadata = archive_metadata(path)
    if metadata.get('pkgname') != [name] or metadata.get('pkgver') != [NAMES[name]] or metadata.get('arch') != ['aarch64']:
        raise ValueError('desktop artifact package identity differs: ' + name)
    if name == 'omarchy' and 'omarchy-settings=4.0.4-2' not in metadata.get('depend', []):
        raise ValueError('Omarchy runtime must require the exact settings version')
    if name == 'aquamarine' and 'libaquamarine.so=14-64' not in metadata.get('provides', []):
        raise ValueError('Aquamarine artifact must provide ABI14')
    contents = package_contents(path)
    for member, digest in item.get('payload', {}).items():
        if contents.get(member, {}).get('sha256') != digest:
            raise ValueError('desktop package payload differs: ' + member)
    return metadata.get('depend', [])


def verify(data, selected, directory):
    dependencies = []
    for name in selected:
        dependencies += verify_archive(name, data['packages'][name], directory)
    dependencies = sorted(set(dependencies) - ({'omarchy-settings=4.0.4-2'} if 'omarchy-settings' in selected else set()))
    if dependencies:
        unmet = command('pacman', '-T', *dependencies, accepted=(0, 127)).strip()
        if unmet:
            raise ValueError('desktop dependencies must already be installed: ' + unmet.replace('\n', ', '))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('plan', 'verify'))
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--root', type=Path, default=Path('/'))
    args = parser.parse_args()
    try:
        if not args.root.is_absolute():
            raise ValueError('desktop inspection root must be absolute')
        data = json.loads(args.manifest.read_text())
        validate_manifest(data)
        selected = plan(data, args.root)
        if args.mode == 'verify':
            if args.directory is None:
                raise ValueError('desktop package directory is required')
            verify(data, selected, args.directory)
        for name in selected:
            item = data['packages'][name]
            print(item['file'] + ' ' + item['sha256'])
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError, subprocess.CalledProcessError, configparser.Error) as error:
        print('Desktop fixes refused: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
