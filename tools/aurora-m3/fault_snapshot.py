"""Retain sealed, bounded M3 firmware coredumps before watchdog recovery.

Reads only kernel-owned immutable devcoredump data, not live GPU memory.
Never acknowledges a dump or changes watchdog/recovery policy.
"""
import hashlib
import json
import os
from pathlib import Path
import struct

MAX_BYTES = 2 * 1024 * 1024
MAGIC = b'M3FWD001'


def decode(raw):
    if not 40 <= len(raw) <= MAX_BYTES or raw[:8] != MAGIC:
        raise ValueError('Invalid M3 fault snapshot header or size')
    count, flags, job, created, length = struct.unpack_from('<IIQQQ', raw, 8)
    if flags != 1 or job != 0 or length != len(raw):
        raise ValueError('Unsealed, truncated or unknown M3 snapshot ABI')
    cursor = 40
    records = []
    for _ in range(count):
        if cursor + 64 > len(raw):
            raise ValueError('Truncated record header')
        name_bytes = raw[cursor:cursor+32]
        name = name_bytes.split(b'\0', 1)[0].decode('ascii')
        if not name or len(name) >= 32 or any(name_bytes[len(name):]):
            raise ValueError('Invalid record name')
        va, size, begin, end = struct.unpack_from('<4Q', raw, cursor+32)
        cursor += 64
        if not size or size > len(raw)-cursor or begin < created or end < begin:
            raise ValueError('Invalid record bounds or time interval')
        records.append(dict(name=name, va=hex(va), size=size, offset=cursor,
                            begin_monotonic_ns=begin, end_monotonic_ns=end,
                            sha256=hashlib.sha256(raw[cursor:cursor+size]).hexdigest()))
        cursor += size
    if cursor != len(raw) or len({r['name'] for r in records}) != count:
        raise ValueError('Trailing bytes or duplicate records')
    return dict(format=MAGIC.decode(), created_monotonic_ns=created,
                length=length, sha256=hashlib.sha256(raw).hexdigest(), records=records,
                note='Sequential owned-firmware RAM observations; not atomic, no shader or application buffers.')


def collect(output, started_ns, root=Path('/sys/class/devcoredump')):
    reports = []
    for source in sorted(root.glob('devcd*/data')):
        with source.open('rb', buffering=0) as f:
            header = f.read(40)
            if header[:8] != MAGIC:
                continue
            if len(header) != 40:
                raise ValueError('Truncated M3 snapshot header')
            created, length = struct.unpack_from('<QQ', header, 24)
            if created < started_ns:
                continue
            if not 40 <= length <= MAX_BYTES:
                raise ValueError('M3 snapshot exceeds collection bound')
            raw = bytearray(header)
            while len(raw) < length:
                part = f.read(min(65536, length-len(raw)))
                if not part:
                    raise ValueError('Truncated M3 snapshot data')
                raw.extend(part)
            if f.read(1):
                raise ValueError('Trailing M3 snapshot data')
        report = decode(raw)
        name = 'm3-fault-' + source.parent.name + '.bin'
        with (output/name).open('xb') as f:
            f.write(raw)
            f.flush()
            os.fsync(f.fileno())
        report.update(file=name, source=str(source))
        with (output/(name+'.json')).open('x') as f:
            f.write(json.dumps(report, indent=2)+'\n')
            f.flush()
            os.fsync(f.fileno())
        reports.append(report)
    return reports
