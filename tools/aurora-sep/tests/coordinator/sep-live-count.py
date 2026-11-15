#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Coordinator-only count assertions; never print device or identity data."""
import argparse
import fcntl
import os
from pathlib import Path
import stat
import struct
import tempfile

INFO_SIZE = 32
GET_INFO = (2 << 30) | (INFO_SIZE << 16) | (0xB1 << 8) | 1


def decode_count(info):
    fields = struct.unpack('=8I', info)
    if fields[0] != 4 or not fields[5] & 1:
        raise RuntimeError('a valid live SEP count is required')
    return fields[6]


def live_count():
    fd = os.open('/dev/sep-bio', os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        if not stat.S_ISCHR(os.fstat(fd).st_mode):
            raise RuntimeError('the biometric character device is required')
        info = bytearray(INFO_SIZE)
        fcntl.ioctl(fd, GET_INFO, info, True)
        return decode_count(info)
    finally:
        os.close(fd)


def private_directory(path):
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
        raise RuntimeError('a root-owned private state directory is required')


def save_expected(path, expected):
    private_directory(path.parent)
    if path.exists() or path.is_symlink():
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
            raise RuntimeError('unsafe assertion state')
    fd, name = tempfile.mkstemp(prefix='.count-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as output:
            output.write(str(expected) + '\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def read_expected(path):
    private_directory(path.parent)
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(fd) as source:
        info = os.fstat(source.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0
                or info.st_mode & 0o077 or info.st_nlink != 1):
            raise RuntimeError('unsafe assertion state')
        text = source.read(32).strip()
    if not text.isdecimal():
        raise RuntimeError('invalid assertion state')
    return int(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['prepare', 'check'])
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--removed', type=int, default=0)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('root is required')
    current = live_count()
    if args.mode == 'prepare':
        if args.removed <= 0 or current < args.removed:
            raise RuntimeError('the enrolled test identity must be present')
        save_expected(args.state, current - args.removed)
    elif current != read_expected(args.state):
        raise RuntimeError('the live enclave count does not match the deletion assertion')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, struct.error):
        raise SystemExit('SEP live-count assertion: FAIL') from None
