#!/usr/bin/env python3
"""Archive unreferenced EFI history while retaining boot and recovery entries."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile

LIMIT = 32 * 1024 * 1024


def regular(path):
    if not stat.S_ISREG(path.lstat().st_mode):
        raise ValueError(f'not a regular file: {path}')
    return path


def checked_tree(root, *, state_metadata=False):
    root = root.absolute()
    for path in (root, *root.parents):
        if path.is_symlink():
            raise ValueError(f'symlink in directory path: {path}')
    if not root.is_dir():
        raise ValueError(f'directory missing: {root}')
    files = []
    for directory, dirs, names in os.walk(root):
        if state_metadata and Path(directory) == root:
            # Saved modules contain header symlinks; archives hold payload bytes.
            # Neither directory supplies boot-reference metadata.
            dirs[:] = [name for name in dirs
                       if not name.startswith('modules-') and name != 'esp-history']
        for name in dirs + names:
            path = Path(directory) / name
            if path.is_symlink():
                raise ValueError(f'symlink in boot metadata: {path}')
        files.extend(Path(directory) / name for name in names)
        if len(files) > 20000:
            raise ValueError('too many boot metadata files')
    return files


def digest(path):
    with regular(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def references(esp, state):
    refs = ''
    total = 0
    for path in checked_tree(esp) + (checked_tree(state, state_metadata=True)
                                    if state.exists() or state.is_symlink() else []):
        if path.suffix.lower() not in ('.conf', '.cfg', '.json', '.var', '.env') and path.name not in ('BOOTAA64.EFI', 'm1n1-good', 'm1n1-failed'):
            continue
        total += regular(path).stat().st_size
        if total > LIMIT:
            raise ValueError('boot metadata exceeds read limit')
        data = path.read_bytes()
        refs += '\n' + data.decode('utf-8', errors='ignore').lower()
        refs += '\n' + data.decode('utf-16-le', errors='ignore').lower()
    if not any((esp / p).is_file() for p in ('EFI/BOOT/limine.conf', 'boot/limine/limine.conf', 'boot/limine.conf', 'limine/limine.conf', 'limine.conf')):
        raise ValueError('Limine configuration missing; history cannot be classified')
    return refs


def inventory(esp, state):
    esp, state = esp.absolute(), state.absolute()
    refs = references(esp, state)
    groups = [[], []]
    for path in checked_tree(esp):
        relative = path.relative_to(esp)
        if len(relative.parts) == 2 and relative.parts[0] == 'm1n1' and re.fullmatch(r'boot\.bin\.before-[A-Za-z0-9._+-]+', relative.name):
            groups[0].append(path)
        elif 'limine_history' in relative.parts[:-1] and re.fullmatch(r'[A-Za-z0-9._+-]+\.efi', relative.name, re.I):
            groups[1].append(path)
    records = []
    for group in groups:
        newest = set(sorted(group, key=lambda p: (regular(p).stat().st_mtime_ns, p.name), reverse=True)[:2])
        for path in sorted(group):
            reason = 'referenced' if path.name.lower() in refs else ('recent' if path in newest else '')
            records.append({'file':str(path.relative_to(esp)), 'bytes':regular(path).stat().st_size,
                            'sha256':digest(path), 'protected':reason})
    return records


@contextlib.contextmanager
def lock(paths):
    fds = []
    try:
        for path in paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
            fds.append(fd)
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise ValueError('nonregular boot lock')
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        for fd in reversed(fds):
            os.close(fd)


def archive(esp, state, destination, lock_paths=None):
    esp, state, destination = esp.absolute(), state.absolute(), destination.absolute()
    destination.mkdir(parents=True, exist_ok=True)
    checked_tree(destination)
    if destination.resolve().is_relative_to(esp.resolve()) or os.stat(destination).st_dev == os.stat(esp).st_dev:
        raise ValueError('archive must be on a different filesystem from the EFI partition')
    with lock(lock_paths or [Path('/run/lock/boot-partition.lock'), Path('/tmp/limine-global.lock')]):
        planned = inventory(esp, state)
        moved = []
        for item in planned:
            if item['protected']:
                continue
            source = esp / item['file']
            target = destination / (item['sha256'] + '.bin')
            if target.exists():
                if digest(target) != item['sha256']:
                    raise ValueError('archive hash collision or damaged archive')
            else:
                fd, name = tempfile.mkstemp(prefix='.esp-history-', dir=destination)
                try:
                    with os.fdopen(fd, 'wb') as stream, regular(source).open('rb') as incoming:
                        while chunk := incoming.read(1024 * 1024):
                            stream.write(chunk)
                        stream.flush()
                        os.fsync(stream.fileno())
                    if digest(Path(name)) != item['sha256']:
                        raise ValueError('history file changed during copy')
                    os.replace(name, target)
                finally:
                    if os.path.exists(name):
                        os.unlink(name)
            # A verified pre-existing archive may still have dirty data pages.
            with regular(target).open('rb') as stream:
                os.fsync(stream.fileno())
            path_key = hashlib.sha256(item['file'].encode()).hexdigest()[:16]
            receipt = destination / (item['sha256'] + '-' + path_key + '.receipt')
            entry = {'original':item['file'], 'sha256':item['sha256'], 'bytes':item['bytes'], 'archive':target.name}
            # Persist restore information before removing the FAT copy.
            fd, name = tempfile.mkstemp(prefix='.esp-receipt-', dir=destination)
            try:
                with os.fdopen(fd, 'w') as stream:
                    json.dump(entry, stream, sort_keys=True);stream.write('\n');stream.flush();os.fsync(stream.fileno())
                os.replace(name, receipt)
            finally:
                if os.path.exists(name):os.unlink(name)
            directory = os.open(destination, os.O_RDONLY | os.O_DIRECTORY)
            try:os.fsync(directory)
            finally:os.close(directory)
            # Re-read references and contents immediately before unlinking.
            current = next((x for x in inventory(esp, state) if x['file'] == item['file']), None)
            if current is None or current['protected'] or current['sha256'] != item['sha256']:
                raise ValueError('boot state changed during archive; original retained')
            source.unlink()
            directory = os.open(source.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:os.fsync(directory)
            finally:os.close(directory)
            moved.append(entry)
        return moved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('report', 'archive'))
    parser.add_argument('--esp', type=Path, required=True)
    parser.add_argument('--state', type=Path, default=Path('/var/lib/aurora-sep'))
    parser.add_argument('--archive', type=Path, default=Path('/var/lib/aurora-sep/esp-history'))
    args = parser.parse_args()
    try:
        if args.action == 'report':
            result = inventory(args.esp, args.state)
        else:
            result = archive(args.esp, args.state, args.archive)
        print(json.dumps(result, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f'EFI history: {error}\n')


if __name__ == '__main__':
    main()
