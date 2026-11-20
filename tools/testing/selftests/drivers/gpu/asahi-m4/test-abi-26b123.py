#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""The 26B123 firmware ABI: production encoders against an independent offset oracle.

check-abi-26b123.rs encodes the same InitData graph, TA/FS/compute work and queue info with
the production 25G83 and 26B123 layouts. This script rebuilds the expected 26B123 bytes from
the 25G83 bytes with explicit per-field tables (old offset -> new offset, transcribed from the
static map of the two firmware builds, not from the production offset runs) and requires an
exact match, and that every non-zero 25G83 byte is accounted for. CPU-only.
"""
import json
from pathlib import Path
import struct
import subprocess
import time
from common import HERE, frozen_bindings, kernel_root, output_dir, rustc, sha

OBJECTS_25G83 = [
    0xc8, 0x4000, 0x4b8, 0xe48, 0x20, 0xeaf0, 0x670, 0x4000, 0x2658, 0x6578, 0x88, 0xc18, 0x1248,
    0xe10, 0x60, 0x1b0, 0x28800, 0x79800, 0x4000, 0x30, 0x4000, 0x30, 0x9000, 0x30, 0x4800, 0x4,
    0x30, 0x4800, 0x4000, 0x4000, 0x4000]
# 26B123 grows GLOBALS (+4), FIRMWARE_DATA (+0x2e0), POWER, MAIN and POWER_PERFORMANCE.
OBJECTS_26B123 = list(OBJECTS_25G83)
OBJECTS_26B123[3] = 0xe4c
OBJECTS_26B123[5] = 0xedd0
OBJECTS_26B123[6] = 0x698
OBJECTS_26B123[8] = 0x2788
OBJECTS_26B123[9] = 0x6c48
ROOT, RUNTIME, GLOBALS, FIRMWARE_DATA, MAIN, POWER_PERFORMANCE = 0, 2, 3, 5, 8, 9


def fields(pairs, width=4):
    return [(old, new, width) for old, new in pairs]


# GLOBALS: one u32 inserted at 0x38; the words the host writes.
GLOBALS_MAP = fields([(0x24, 0x24), (0x78, 0x7c), (0x88, 0x8c), (0x8c, 0x90), (0x98, 0x9c),
                      (0x998, 0x99c), (0x99c, 0x9a0), (0x9a0, 0x9a4), (0x9b8, 0x9bc),
                      (0x9bc, 0x9c0), (0x9c0, 0x9c4), (0x9c8, 0x9cc), (0x9cc, 0x9d0),
                      (0xe28, 0xe2c)])
# FirmwareData: FWCTL state/ring pointers and the scheduler word.
FIRMWARE_DATA_MAP = [(0x4f88, 0x5168, 8), (0x4f90, 0x5170, 8), (0x4fd4, 0x51b4, 4)]
# HW data: the G16-written words, then the register map slot renumbering.
MAIN_MAP = [(0x28, 0x28, 8), (0xe88, 0xfa0, 8), (0xf24, 0x1040, 8)] + fields([
    (0xea0, 0xfc0), (0xea4, 0xfc4), (0xeb8, 0xfd8), (0xed0, 0xfec), (0xed4, 0xff0),
    (0xed8, 0xff4), (0xee0, 0xffc), (0xfc0, 0x10dc), (0xfc4, 0x10e0), (0x2544, 0x2660),
    (0x2548, 0x2664), (0x254c, 0x2668), (0x2550, 0x266c), (0x2554, 0x2670), (0x2568, 0x2684),
    (0x2570, 0x268c), (0x259c, 0x26b8)])
MAIN_MAP += fields([(0xfc8 + 4 * i, 0x10e4 + 4 * i) for i in range(16)])
MAIN_MAP += fields([(0x1808 + 4 * i, 0x1924 + 4 * i) for i in range(16)])
MAIN_MAP += fields([(0x1008 + 4 * k, 0x1124 + 4 * k) for k in range(256)])
MAIN_MAP += fields([(0x1408 + 4 * k, 0x1524 + 4 * k) for k in range(256)])
SLOTS = {0: 0, 3: 3, 9: 9, 10: 10, 12: 12, 14: 14, 15: 15, 17: 17, 22: 22, 26: 31, 27: 32,
         28: 33, 29: 34, 31: 36, 32: 37, 36: 41, 39: 44, 40: 45, 41: 46}
MAIN_MAP += [(0x640 + old * 0x28, 0x640 + new * 0x28, 0x28) for old, new in SLOTS.items()]
# Power config: the words the host writes and the temperature pointer.
POWER_MAP = [(0x3b50, 0x3b98, 8)] + fields([
    (0x4, 0x4), (0x8, 0x8), (0x10, 0x10), (0x14, 0x14), (0x2c, 0x2c), (0x30, 0x30),
    (0x9f0, 0xa00), (0x9f4, 0xa04), (0xa38, 0xa48), (0xaa8, 0xab8), (0xaac, 0xabc),
    (0xaf0, 0xb00), (0xb78, 0xb88), (0xb7c, 0xb8c), (0xb80, 0xb90), (0xbc0, 0xbd0),
    (0x2210, 0x2220), (0x22b0, 0x22c0), (0x22b4, 0x22c4), (0x22f8, 0x2308), (0x279e, 0x27ae),
    (0x2870, 0x2880), (0x2a3c, 0x2a64), (0x2b84, 0x2bac), (0x2b88, 0x2bb0), (0x2b8c, 0x2bb4),
    (0x2bcc, 0x2bf4), (0x2c40, 0x2c68), (0x2c44, 0x2c6c), (0x2c88, 0x2cb0)])
# Words new in 26B123 that the host writes (none: HW data +0xfa8 stays zero, MAP section 8).
NEW_26B123 = {}
MOVED = {GLOBALS: GLOBALS_MAP, FIRMWARE_DATA: FIRMWARE_DATA_MAP, MAIN: MAIN_MAP,
         POWER_PERFORMANCE: POWER_MAP,
         # Main config: only the power-config pointer moves (16 bytes later).
         RUNTIME: [(0, 0, 0x469), (0x469, 0x479, 8)]}


def split(image, sizes):
    out, at = [], 0
    for size in sizes:
        out.append(image[at:at + size])
        at += size
    assert at == len(image), (at, len(image))
    return out


def moved(old, size, table, name):
    """New object of `size` bytes holding `old`'s fields at their new offsets; every non-zero
    byte of `old` must belong to some field."""
    new, covered = bytearray(size), bytearray(len(old))
    for at, to, width in table:
        new[to:to + width] = old[at:at + width]
        covered[at:at + width] = b'\1' * width
    stray = [hex(i) for i, (b, c) in enumerate(zip(old, covered)) if b and not c]
    assert not stray, f'{name}: 25G83 bytes without a 26B123 position: {stray[:8]}'
    return new


def expect_initdata(old_image):
    old = split(old_image, OBJECTS_25G83)
    new = []
    for index, (before, size) in enumerate(zip(old, OBJECTS_26B123)):
        if index == ROOT:
            assert struct.unpack_from('<Q', before)[0] == 0x0c89c357839204b8
            after = bytearray(before)
            struct.pack_into('<Q', after, 0, 0x0c8a836f039204b8)
        elif index in MOVED:
            after = moved(before, size, MOVED[index], f'object {index}')
        elif size != len(before):
            # POWER (power state) grows; the host leaves it all zero in both ABIs.
            assert not any(before), f'object {index}'
            after = bytearray(size)
        else:
            after = bytearray(before)
        for at, value in NEW_26B123.get(index, []):
            assert not any(after[at:at + 8]), f'object {index} +{at:#x}'
            struct.pack_into('<Q', after, at, value)
        new.append(bytes(after))
    return b''.join(new)


def plus7(image, at):
    """The u64 work pointer at `at` points seven bytes further on."""
    value = struct.unpack_from('<Q', image, at)[0]
    struct.pack_into('<Q', image, at, value + 7)


def widen(image, at):
    """A u8 generation becomes a u64 at the same offset (its next 7 bytes were free)."""
    assert image[at + 1:at + 8] == bytes(7), hex(at)


def expect_render(old):
    ta_work, ta_seq, fs_work, fs_seq = 0x940, 0x300, 0xca0, 0x380
    tw = bytearray(old[:ta_work])
    tq = bytearray(old[ta_work:ta_work + ta_seq])
    fw = bytearray(old[ta_work + ta_seq:ta_work + ta_seq + fs_work])
    fq = bytearray(old[ta_work + ta_seq + fs_work:])
    assert len(fq) == fs_seq
    widen(tw, 0x917)
    for at in (0x268 + 0x85, 0x1cc + 0x2c, 0x21c + 0x2c):
        plus7(tq, at)
    widen(fw, 0xc5f)
    fw[0xc8b], fw[0xc84] = fw[0xc84], 0
    for at in (0x288 + 0xa5, 0x1ec + 0x2c, 0x23c + 0x2c):
        plus7(fq, at)
    return bytes(tw + tq + fw + fq)


def expect_compute(old):
    work, seq, embedded = 0x2000, 0x300, 0x900
    w = bytearray(old[:work])
    q = bytearray(old[work:])
    assert len(q) == seq and w[embedded:embedded + seq] == q
    widen(w, 0x88b)
    for at in (0x1a8, 0x1bc + 0x2c, 0x20c + 0x2c, 0x258 + 0x69, 0x258 + 0x71):
        plus7(q, at)
        plus7(w, embedded + at)
    return bytes(w + q)


def expect_queue(old):
    """26B123 queue info: 0x80-byte header, the words after 0x40 moved down, the scheduling
    context aligned at 0x78, the scratch after the header."""
    new = bytearray(len(old))
    new[0:0x18] = old[0:0x18]
    struct.pack_into('<Q', new, 0x18, struct.unpack_from('<Q', old, 0x18)[0] - 0xb0 + 0x80)
    new[0x2c:0x40] = old[0x2c:0x40]
    new[0x44:0x48] = old[0x48:0x4c]
    new[0x48:0x4c] = old[0x4c:0x50]
    new[0x78:0x80] = old[0xa4:0xac]
    assert not any(old[0x40:0x48]) and not any(old[0x50:0xa4]) and not any(old[0xac:])
    return bytes(new)


def main():
    start = time.monotonic()
    root, out = kernel_root(), output_dir()
    (out / 'public-uapi.rs').write_bytes(frozen_bindings().read_bytes())
    identity = (root / 'drivers/gpu/drm/asahi/fwid.rs').read_text()
    first = identity.index('pub(crate) struct FirmwareBuild {')
    source = (HERE / 'check-abi-26b123.rs').read_text().replace('@KERNEL@', str(root))
    source = source.replace('@FWID_BUILD@', identity[first:identity.index('\n}', first) + 2])
    harness = out / 'check-abi-26b123.rs'
    harness.write_text(source)
    binary = out / 'check-abi-26b123'
    for command in [rustc() + ['--edition=2021', '-Dwarnings', '-C', 'overflow-checks=yes',
                               str(harness), '-o', str(binary)], [str(binary), str(out)]]:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        if result.returncode:
            return result.returncode
    checked = 0
    for name, expect in [('initdata', expect_initdata), ('render', expect_render),
                         ('compute', expect_compute)]:
        old = (out / f'{name}-25g83.bin').read_bytes()
        new = (out / f'{name}-26b123.bin').read_bytes()
        wanted = expect(old)
        assert len(new) == len(wanted), (name, len(new), len(wanted))
        diff = next((i for i, (a, b) in enumerate(zip(new, wanted)) if a != b), None)
        assert diff is None, f'{name}: 26B123 byte {diff:#x} is {new[diff]:#x}, oracle {wanted[diff]:#x}'
        checked += len(new)
    queue = (out / 'queue.bin').read_bytes()
    assert queue[0xb4:] == expect_queue(queue[:0xb4]), 'queue info'
    checked += 0xb4
    print(f'26B123 encodings match the independent offset oracle: {checked} bytes '
          '(InitData, TA/FS/compute work and sequences, queue info)')
    report = {'passed': True, 'cpu_only': True, 'hardware_execution': False,
              'scope': 'Production 26B123 InitData/work/queue encoders versus explicit per-field '
                       'offset tables derived from the 25G83 encoding; no firmware execution.',
              'compared_bytes': checked, 'seconds': round(time.monotonic() - start, 3),
              'source_sha256': {name: sha(root / 'drivers/gpu/drm/asahi/g16' / name) for name in
                                ['hw.rs', 'fw/mod.rs', 'fw/initdata.rs', 'fw/channels.rs',
                                 'fw/graph.rs', 'fw/work.rs', 'fw/work/compute.rs', 'encode.rs',
                                 'encode/render.rs', 'encode/compute.rs']}}
    (out / 'abi-26b123.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
