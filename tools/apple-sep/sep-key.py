#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Example client for application-owned SEP biometric P-256 keys."""

import argparse
import base64
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time

MAX_BLOB = 4096
BIOMETRIC_KEYS = 2
BIOMETRIC_DECRYPT = 4


def ioctl_number(direction, number, size=0):
    return (direction << 30) | (size << 16) | (0xB1 << 8) | number


GET_INFO = ioctl_number(2, 1, 32)
CANCEL = ioctl_number(0, 7)
CREATE = ioctl_number(3, 11, 88)
SIGN_START = ioctl_number(1, 12, 56)
SIGN_POLL = ioctl_number(2, 13, 88)
DECRYPT_START = ioctl_number(1, 14, 32)
DECRYPT_POLL = ioctl_number(3, 15, 24)


class SepClient:
    def __enter__(self):
        self.fd = os.open('/dev/sep-bio', os.O_RDWR | os.O_CLOEXEC)
        try:
            info = bytearray(32)
            fcntl.ioctl(self.fd, GET_INFO, info, True)
            self.capabilities = struct.unpack('=8I', info)[5]
            if not self.capabilities & BIOMETRIC_KEYS:
                raise RuntimeError('driver does not advertise biometric keys')
        except BaseException:
            os.close(self.fd)
            raise
        return self

    def __exit__(self, *unused):
        os.close(self.fd)

    def create(self, decrypt=False):
        if decrypt and not self.capabilities & BIOMETRIC_DECRYPT:
            raise OSError(errno.EOPNOTSUPP, 'driver does not advertise biometric decryption')
        blob = ctypes.create_string_buffer(MAX_BLOB)
        request = bytearray(struct.pack('=IIQ65s7x', int(decrypt), MAX_BLOB,
                                        ctypes.addressof(blob), b''))
        fcntl.ioctl(self.fd, CREATE, request, True)
        _, size, _, public = struct.unpack('=IIQ65s7x', request)
        if not 0 < size <= MAX_BLOB or public[0] != 4:
            raise RuntimeError('invalid key response')
        return bytes(blob[:size]), public

    def decrypt_into(self, blob, sealed, output, timeout_ms=60000):
        """Return length in caller-owned bytearray; wipe it on any failure.

        The caller must wipe successful output after use. No implicit fallback,
        raw firmware context, automatic retry, or plaintext file is involved.
        """
        if not isinstance(output, bytearray) or len(output) != MAX_BLOB:
            raise ValueError('output must be a 4096-byte bytearray')
        destination = (ctypes.c_ubyte * MAX_BLOB).from_buffer(output)
        ctypes.memset(ctypes.addressof(destination), 0, MAX_BLOB)
        if not self.capabilities & BIOMETRIC_DECRYPT:
            raise OSError(errno.EOPNOTSUPP, 'driver does not advertise biometric decryption')
        if not 0 < len(blob) <= MAX_BLOB or not 81 <= len(sealed) <= MAX_BLOB:
            raise ValueError('invalid wrapped key or sealed length')
        if sealed[0] != 4 or not 1 <= timeout_ms <= 120000:
            raise ValueError('expected uncompressed P-256 point and valid timeout')
        key = ctypes.create_string_buffer(blob)
        ciphertext = ctypes.create_string_buffer(sealed)
        request = struct.pack('=IIIIQQ', 0, timeout_ms, len(blob), len(sealed),
                              ctypes.addressof(key), ctypes.addressof(ciphertext))
        deadline = time.monotonic() + timeout_ms / 1000
        finished = False
        success = False
        started = False
        try:
            fcntl.ioctl(self.fd, DECRYPT_START, request)
            started = True
            poller = select.poll()
            poller.register(self.fd, select.POLLIN | select.POLLERR | select.POLLHUP)
            while time.monotonic() < deadline:
                response = bytearray(struct.pack('=IiIIQ', 0, 0, 0, MAX_BLOB,
                                                ctypes.addressof(destination)))
                fcntl.ioctl(self.fd, DECRYPT_POLL, response, True)
                state, status, guidance, size, unused = struct.unpack('=IiIIQ', response)
                if state == 4:
                    finished = True
                    raise OSError(-status, os.strerror(-status))
                if state == 3:
                    finished = True
                    if not 0 <= size <= MAX_BLOB:
                        raise RuntimeError('invalid plaintext length')
                    success = True
                    return size
                events = poller.poll(min(500, max(1, int((deadline - time.monotonic()) * 1000))))
                if any(mask & (select.POLLERR | select.POLLHUP) for _, mask in events):
                    raise RuntimeError('SEP disconnected; completion is unknown')
            raise TimeoutError('decryption expired')
        finally:
            if not success:
                ctypes.memset(ctypes.addressof(destination), 0, MAX_BLOB)
            if started and not finished:
                try:
                    fcntl.ioctl(self.fd, CANCEL)
                except OSError:
                    pass

    def sign_digest(self, blob, digest, timeout_ms=60000):
        if not 0 < len(blob) <= MAX_BLOB or len(digest) != 32:
            raise ValueError('expected wrapped key and SHA-256 digest')
        if not 1 <= timeout_ms <= 120000:
            raise ValueError('timeout must be 1..120000 ms')
        storage = ctypes.create_string_buffer(blob)
        request = struct.pack('=IIIIQ32s', 0, timeout_ms, len(blob), 0,
                              ctypes.addressof(storage), digest)
        fcntl.ioctl(self.fd, SIGN_START, request)
        deadline = time.monotonic() + timeout_ms / 1000
        poller = select.poll()
        poller.register(self.fd, select.POLLIN | select.POLLERR | select.POLLHUP)
        finished = False
        try:
            while time.monotonic() < deadline:
                response = bytearray(88)
                fcntl.ioctl(self.fd, SIGN_POLL, response, True)
                state, status, guidance, size, signature = struct.unpack('=IiII72s', response)
                if state == 4:
                    finished = True
                    raise OSError(-status, os.strerror(-status))
                if state == 3:
                    finished = True
                    if not 0 < size <= 72:
                        raise RuntimeError('invalid signature length')
                    return signature[:size]
                events = poller.poll(min(500, max(1, int((deadline - time.monotonic()) * 1000))))
                if any(mask & (select.POLLERR | select.POLLHUP) for _, mask in events):
                    raise RuntimeError('SEP disconnected; completion is unknown')
            raise TimeoutError('signing expired')
        finally:
            if not finished:
                try:
                    fcntl.ioctl(self.fd, CANCEL)
                except OSError:
                    pass
                # Do not automatically retry a request with uncertain completion.


def verify(public, digest, signature):
    # SubjectPublicKeyInfo for id-ecPublicKey / prime256v1, uncompressed point.
    prefix = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')
    with tempfile.TemporaryDirectory(prefix='sep-signature-') as directory:
        directory = Path(directory)
        for name, data in [('public.der', prefix + public), ('digest', digest),
                           ('signature', signature)]:
            (directory / name).write_bytes(data)
        subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin', '-keyform', 'DER',
                        '-inkey', str(directory / 'public.der'),
                        '-in', str(directory / 'digest'),
                        '-sigfile', str(directory / 'signature'),
                        '-pkeyopt', 'digest:sha256'], check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    create = commands.add_parser('create')
    create.add_argument('key', type=Path)
    create.add_argument('--decrypt', action='store_true',
                        help='create a biometric signing/decryption key')
    for name in ('sign', 'sign-digest'):
        sign = commands.add_parser(name)
        sign.add_argument('key', type=Path)
        sign.add_argument('message', type=Path)
        sign.add_argument('signature', type=Path)
    args = parser.parse_args()
    if args.command == 'create':
        if args.key.exists():
            raise FileExistsError(args.key)
        with SepClient() as client:
            blob, public = client.create(decrypt=args.decrypt)
        data = {'version': 1, 'policy': 'biometry-any',
                'blob': base64.b64encode(blob).decode(),
                'public': base64.b64encode(public).decode()}
        if args.decrypt:
            data['operations'] = ['sign', 'decrypt']
        fd = os.open(args.key, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
        with os.fdopen(fd, 'w') as output:
            json.dump(data, output)
            output.flush()
            os.fsync(output.fileno())
        print('Biometric P-256 key created')
    else:
        if args.key.stat().st_size > 8192:
            raise ValueError('key file too large')
        data = json.loads(args.key.read_text())
        if data['version'] != 1 or data['policy'] != 'biometry-any':
            raise ValueError('unsupported key file')
        blob = base64.b64decode(data['blob'], validate=True)
        public = base64.b64decode(data['public'], validate=True)
        with args.message.open('rb') as message:
            if args.command == 'sign':
                digest = hashlib.file_digest(message, 'sha256').digest()
            else:
                digest = message.read(33)
                if len(digest) != 32:
                    raise ValueError('sign-digest requires exactly 32 raw SHA-256 bytes')
        print('Place an enrolled finger on Touch ID', flush=True)
        with SepClient() as client:
            signature = client.sign_digest(blob, digest)
        verify(public, digest, signature)
        args.signature.write_bytes(signature)
        print('Biometric signature verified')


if __name__ == '__main__':
    main()
