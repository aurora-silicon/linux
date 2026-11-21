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
ROOT, RUNTIME, GLOBALS, FIRMWARE_DATA, POWER, MAIN, POWER_PERFORMANCE = 0, 2, 3, 5, 6, 8, 9
RETENTION = 28


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
MAIN_MAP = [(0x28, 0x28, 8), (0xf24, 0x1040, 8)] + fields([
    (0xea0, 0xfc0), (0xea4, 0xfc4), (0xeb8, 0xfd8), (0xed0, 0xfec), (0xed4, 0xff0),
    (0xed8, 0xff4), (0xee0, 0xffc), (0xfc0, 0x10dc), (0xfc4, 0x10e0), (0x2544, 0x2660),
    (0x2548, 0x2664), (0x254c, 0x2668), (0x2550, 0x266c), (0x2554, 0x2670), (0x2568, 0x2684),
    (0x2570, 0x268c), (0x259c, 0x26b8)])
MAIN_MAP += fields([(0xfc8 + 4 * i, 0x10e4 + 4 * i) for i in range(16)])
MAIN_MAP += fields([(0x1808 + 4 * i, 0x1924 + 4 * i) for i in range(16)])
MAIN_MAP += fields([(0x1008 + 4 * k, 0x1124 + 4 * k) for k in range(256)])
MAIN_MAP += fields([(0x1408 + 4 * k, 0x1524 + 4 * k) for k in range(256)])
SLOTS = {0: 0, 3: 3, 9: 9, 10: 10, 12: 12, 14: 14, 15: 15, 17: 17, 22: 22, 26: 31, 27: 32,
         28: 33, 29: 34, 31: 36, 32: 37, 39: 44, 40: 45, 41: 46}
MAIN_MAP += [(0x640 + old * 0x28, 0x640 + new * 0x28, 0x28) for old, new in SLOTS.items()]
# Logical 36 (DFenderPowerTrans, slot 41) stays empty on 26B123, as the M4 Pro leaves it:
# its 25G83 bytes are accounted for without a 26B123 position. The retention pointer (25G83
# MAIN 0xe88) leaves MAIN: macOS 27.2 publishes none, and the object is RUNTIME's 0x2c8 table.
DROPPED = {MAIN: [(0x640 + 36 * 0x28, 0x28), (0xe88, 8)]}
# Power config: the words the host writes and the temperature pointer.
POWER_MAP = [(0x3b50, 0x3b98, 8)] + fields([
    (0x4, 0x4), (0x8, 0x8), (0x10, 0x10), (0x14, 0x14), (0x2c, 0x2c), (0x30, 0x30),
    (0x9f0, 0xa00), (0x9f4, 0xa04), (0xa38, 0xa48), (0xaa8, 0xab8), (0xaac, 0xabc),
    (0xaf0, 0xb00), (0xb78, 0xb88), (0xb7c, 0xb8c), (0xb80, 0xb90), (0xbc0, 0xbd0),
    (0x2210, 0x2220), (0x22b0, 0x22c0), (0x22b4, 0x22c4), (0x22f8, 0x2308), (0x279e, 0x27ae),
    (0x2870, 0x2880), (0x2a3c, 0x2a64), (0x2b84, 0x2bac), (0x2b88, 0x2bb0), (0x2b8c, 0x2bb4),
    (0x2bcc, 0x2bf4), (0x2c40, 0x2c68), (0x2c44, 0x2c6c), (0x2c88, 0x2cb0)])
# Words new in 26B123 that the host writes: the u64 crash copy buffer size at HW data 0xfa8.
NEW_26B123 = {MAIN: [(0xfa8, 0x8000)]}
# macOS 27.2 J773g InitData (hv25): {object: [(offset, count, u32)]}, `count` consecutive words.
MACOS_27_2 = {
    3: [(0x30, 3, 0x1), (0x40, 1, 0x78), (0x58, 1, 0x28ffff), (0x5c, 1, 0xffff),
         (0x60, 2, 0x10000), (0x68, 1, 0x30000), (0x6c, 1, 0x10000), (0x84, 1, 0x241d),
         (0x88, 1, 0x91ad), (0x7c8, 1, 0x4e2), (0x7cc, 1, 0x3f800000), (0x7d0, 1, 0x3f4ccccd),
         (0x7d4, 1, 0x3e4ccccd), (0x7d8, 1, 0x3f666666), (0x7dc, 1, 0x3dcccccd),
         (0x7e0, 1, 0x3e800000), (0x7e4, 1, 0x3f333333), (0x7e8, 1, 0x3f666666), (0x7ec, 1, 0x6),
         (0x7f0, 2, 0x1), (0x7f8, 2, 0x64), (0x9a8, 1, 0x1), (0x9d4, 1, 0x1), (0x9dc, 1, 0x32),
         (0xe0c, 1, 0xfa), (0xe30, 1, 0x1)],
    6: [(0x10, 1, 0x1f), (0x14, 1, 0x3e0a3d71), (0x18, 1, 0x401ae148), (0x1c, 1, 0x28),
         (0x20, 1, 0x1), (0xc8, 1, 0x2af8), (0xcc, 1, 0x1f40), (0xd8, 1, 0x42f00000),
         (0xdc, 1, 0x3fd55554), (0xf0, 1, 0x27a6), (0xf4, 1, 0xc8), (0x108, 1, 0x437a0000),
         (0x10c, 1, 0x3f933333), (0x110, 1, 0x271), (0x12c, 1, 0x91ad), (0x130, 1, 0x3e800000),
         (0x134, 1, 0x3f99999a), (0x624, 1, 0x41700000), (0x628, 1, 0x40a00000), (0x62c, 1, 0x20),
         (0x630, 1, 0x3e8), (0x640, 1, 0x384)],
    8: [(0xfb0, 1, 0x8132), (0xfb4, 2, 0x1), (0xfdc, 1, 0x1), (0xfe4, 1, 0x1), (0x1000, 2, 0x1),
         (0x1020, 1, 0x1f), (0x1044, 1, 0xc), (0x1050, 2, 0x1), (0x1068, 1, 0x31), (0x1088, 1, 0x1),
         (0x10a4, 1, 0xa), (0x10a8, 1, 0x3), (0x10d4, 1, 0x12), (0x10d8, 1, 0x4), (0x10dc, 1, 0xa),
         (0x1964, 11, 0x3f828f5c), (0x2684, 1, 0xa), (0x26a8, 1, 0x160000), (0x26b0, 2, 0x1),
         (0x26c0, 3, 0x1), (0x26d0, 12, 0xffffffff), (0x2724, 1, 0x1), (0x2754, 1, 0x1)],
    9: [(0x0, 1, 0x1), (0x4, 1, 0x30d3f), (0x2c, 2, 0x3), (0x40, 1, 0x12c), (0x44, 1, 0xc),
         (0x48, 1, 0x3e8), (0x50, 1, 0x12c), (0x60, 1, 0x4), (0x64, 1, 0x3f800000), (0x6c, 2, 0x1),
         (0x80, 1, 0x12c), (0x84, 1, 0x1), (0x88, 1, 0x3e8), (0x90, 1, 0x64), (0x9c, 4, 0x1),
         (0xd0, 11, 0x3f828f5c), (0x210, 1, 0x1), (0x9c8, 1, 0x271), (0x9d4, 1, 0x3f7f2e9f),
         (0x9dc, 1, 0x3b516154), (0x9e4, 1, 0x3ca59586), (0x9f0, 1, 0x4711ad00),
         (0x9f4, 1, 0x40a90fdb), (0x9fc, 1, 0xbcdbaa78), (0xa00, 1, 0x28), (0xa08, 1, 0x3e8),
         (0xa14, 1, 0x91ad), (0xa80, 1, 0x4), (0xa94, 1, 0x3f800000), (0xa9c, 1, 0x3f99999a),
         (0xaa8, 1, 0x47800000), (0xaac, 1, 0x3e800000), (0xab8, 1, 0x28), (0xacc, 1, 0x91ad),
         (0xad8, 1, 0x10), (0xb0c, 1, 0x3f800000), (0xb10, 1, 0x3f99999a), (0xb14, 1, 0x3e800000),
         (0xb38, 1, 0x55), (0xb3c, 1, 0x2), (0xb40, 1, 0x5f), (0xb44, 1, 0x32), (0xb48, 1, 0x6),
         (0xb50, 1, 0x6), (0xb54, 1, 0x1), (0xb5c, 1, 0x3f4ccccd), (0xb60, 1, 0x3f7eb852),
         (0xb64, 1, 0x3e4ccccd), (0xb68, 1, 0x3ba3d70a), (0xb6c, 2, 0x3f4c28f6),
         (0xb78, 1, 0x42be0000), (0xb7c, 2, 0x40accccd), (0xb84, 1, 0x40ebca1b), (0xb88, 1, 0x12c),
         (0xb90, 1, 0x12c), (0xb98, 1, 0x42aa0000), (0xb9c, 1, 0x55), (0xbd0, 1, 0x12c),
         (0xbd8, 1, 0x3f4ccccd), (0xbdc, 1, 0x3e4ccccd), (0x1bfc, 1, 0xff000000),
         (0x1c00, 255, 0xff7fff7f), (0x1ffc, 1, 0x7fff7f), (0x2200, 1, 0x47800000),
         (0x2204, 1, 0x43960000), (0x2208, 1, 0x447a0000), (0x220c, 1, 0x62a), (0x2220, 1, 0x12c),
         (0x2228, 1, 0x42c80000), (0x222c, 1, 0x3e8), (0x2234, 1, 0x3f4ccccd),
         (0x2238, 1, 0x3e4ccccd), (0x2274, 1, 0x1092), (0x2278, 1, 0x6958), (0x2280, 1, 0x1f40),
         (0x2288, 1, 0x4), (0x229c, 1, 0x3f800000), (0x22a4, 1, 0x3fd55554),
         (0x22b0, 1, 0x47800000), (0x22b4, 1, 0x42f00000), (0x22c0, 1, 0x28), (0x22c8, 1, 0x3e8),
         (0x22d4, 1, 0x2af8), (0x22d8, 1, 0x1092), (0x2528, 1, 0x101), (0x2728, 1, 0xe030000),
         (0x2744, 1, 0x3f80), (0x2748, 1, 0x33330000), (0x274c, 1, 0x3f93), (0x2758, 1, 0x4780),
         (0x275c, 1, 0x437a), (0x2768, 2, 0x3e80000), (0x277c, 1, 0xe030000),
         (0x27bc, 1, 0x33333f80), (0x27c0, 1, 0x3f93), (0x27c4, 1, 0x437a), (0x27c8, 1, 0xc80000),
         (0x27dc, 1, 0x42d7), (0x27e0, 1, 0x2bc42d7), (0x27e8, 1, 0x4280), (0x27ec, 1, 0x4780),
         (0x2878, 1, 0x428), (0x2884, 1, 0x31c), (0x2888, 1, 0x26a), (0x28d8, 2, 0x42c80000),
         (0x28e8, 1, 0x1), (0x28f0, 3, 0x91ad), (0x2900, 1, 0x4711ad00), (0x2908, 1, 0x3f800000),
         (0x290c, 1, 0x4), (0x2910, 1, 0x8), (0x2914, 1, 0x2ee00), (0x29e4, 1, 0x74),
         (0x29f0, 1, 0x3f772c23), (0x29f8, 1, 0x3d0d3dcb), (0x2a00, 1, 0x3e0a3d71),
         (0x2a0c, 1, 0x47800000), (0x2a10, 1, 0x401ae148), (0x2a1c, 1, 0x28), (0x2a20, 2, 0x3e8),
         (0x2a2c, 1, 0x40a11b75), (0x2a30, 1, 0x91ad), (0x2a3c, 1, 0xf8), (0x2a44, 1, 0x5ad200),
         (0x2a6c, 1, 0x3f772c23), (0x2a70, 1, 0x3d0d3dcb), (0x2a74, 1, 0x3e0a3d71),
         (0x2a78, 1, 0x401ae148), (0x2b48, 1, 0x208d), (0x2b68, 1, 0x1), (0x2b74, 1, 0x32),
         (0x2b78, 1, 0x1), (0x2b80, 1, 0x3f638e39), (0x2b84, 1, 0x3f2aaaab),
         (0x2b88, 1, 0x3de38e39), (0x2b8c, 1, 0x3eaaaaab), (0x2b90, 1, 0xbecccccd),
         (0x2b94, 1, 0xbf4ccccd), (0x2b9c, 1, 0x47800000), (0x2ba0, 2, 0xc0a00000),
         (0x2bac, 1, 0x64), (0x2bb4, 1, 0x12c), (0x2bbc, 1, 0x46023400), (0x2bc0, 1, 0x578),
         (0x2bcc, 1, 0x48), (0x2bd0, 1, 0x18), (0x2bd4, 1, 0x1a5e00), (0x2bdc, 1, 0x8ca00),
         (0x2be8, 2, 0x47800000), (0x2c10, 1, 0x208d), (0x2c14, 1, 0x4), (0x2c18, 1, 0x9c4),
         (0x2c1c, 1, 0x258), (0x2c20, 1, 0x2), (0x2c24, 1, 0x4), (0x2c28, 1, 0x32),
         (0x2c58, 1, 0x47800000), (0x2c68, 1, 0x28), (0x2d14, 2, 0x1), (0x2d24, 1, 0x6),
         (0x2e4c, 1, 0x4), (0x2e58, 1, 0x3f780000), (0x2e60, 1, 0x3d000000),
         (0x2e68, 1, 0x40a00000), (0x2e74, 1, 0x47800000), (0x2e78, 1, 0x41700000),
         (0x2e84, 1, 0x384), (0x2e88, 2, 0x3e8), (0x2e98, 1, 0x3e8), (0x2ecc, 1, 0x3e8),
         (0x2f0c, 1, 0x42c80000), (0x2f28, 1, 0x6958), (0x2f30, 1, 0x7d), (0x2f34, 1, 0x1),
         (0x3ba0, 1, 0x6b79), (0x3ba8, 1, 0x1), (0x5bac, 1, 0x4159999a), (0x5bb0, 1, 0x41000000),
         (0x6348, 1, 0x67305452), (0x6388, 1, 0x1), (0x63f0, 1, 0x1), (0x6ac8, 1, 0x1)],
}
# Unaligned and byte-wide macOS words: (object, offset, bytes).
MACOS_27_2_BYTES = [(MAIN, 0x277e, (1).to_bytes(4, 'little')), (MAIN, 0x2782, b'\x01'),
                    (RUNTIME, 0x3d8, b'\xff')]
# macOS words that replace a non-zero word of the remapped 25G83 encoding (ceiling 10, 11
# rows, as check-abi-26b123.rs encodes): {object: {offset: the remapped value}}.
REPLACED = {
    MAIN: {0x1044: 6, 0x10dc: 11, 0x2684: 8},
    POWER_PERFORMANCE: {0x4: 200000, 0x2c: 10, 0x30: 10, 0xa00: 1000, 0xab8: 1000, 0xb88: 1000,
                        0xb90: 1000, 0xbd0: 1000, 0x2220: 1000, 0x22c0: 1000, 0x2bac: 1000,
                        0x2bb4: 1000, 0x2c68: 1000},
}
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


def name_index(name):
    return int(name.split()[1]) if name.startswith('object ') else -1


def moved(old, size, table, name):
    """New object of `size` bytes holding `old`'s fields at their new offsets; every non-zero
    byte of `old` must belong to some field."""
    new, covered = bytearray(size), bytearray(len(old))
    for at, to, width in table:
        new[to:to + width] = old[at:at + width]
        covered[at:at + width] = b'\1' * width
    for at, width in DROPPED.get(name_index(name), []):
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
            # POWER (power state) grows; the host leaves it all zero in 25G83.
            assert not any(before), f'object {index}'
            after = bytearray(size)
        else:
            after = bytearray(before)
        if index == MAIN:
            # The auxiliary DVFS copy (25G83 0x1cd8 -> 0x1df4): the top row, then each row's
            # auxiliary frequency, lane-0 core and SRAM voltages.
            ceiling = struct.unpack_from('<I', before, 0xfc4)[0]
            struct.pack_into('<I', after, 0x1df4, ceiling)
            for i in range(ceiling + 1):
                after[0x1df8 + 4 * i:0x1dfc + 4 * i] = before[0x1808 + 4 * i:0x180c + 4 * i]
                after[0x1e38 + 0x40 * i:0x1e3c + 0x40 * i] = before[0x1008 + 0x40 * i:0x100c + 0x40 * i]
                after[0x2238 + 0x40 * i:0x223c + 0x40 * i] = before[0x1408 + 0x40 * i:0x140c + 0x40 * i]
        if index == RUNTIME:
            # The RETENTION object (25G83 MAIN 0xe88) is the zeroed table at RUNTIME 0x2c8.
            assert not any(after[0x2c8:0x2d0])
            after[0x2c8:0x2d0] = old[MAIN][0xe88:0xe90]
        for at, count, value in MACOS_27_2.get(index, []):
            for n in range(count):
                word = at + 4 * n
                was = struct.unpack_from('<I', after, word)[0]
                assert was == REPLACED.get(index, {}).get(word, 0), f'object {index} +{word:#x}: {was:#x}'
                struct.pack_into('<I', after, word, value)
        for obj, at, data in MACOS_27_2_BYTES:
            if obj == index:
                assert not any(after[at:at + len(data)]), f'object {index} +{at:#x}'
                after[at:at + len(data)] = data
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
    # 26B123 appends the free-list slot record {0x14028, the descriptor's slot word} to the
    # register list at +0x20, as macOS 27.2 sends it.
    count = struct.unpack_from('<H', w, 0x728)[0]
    struct.pack_into('<IQ', w, 0x20 + count * 12, 0x14028, struct.unpack_from('<I', w, 0xe00 + 8)[0])
    struct.pack_into('<HH', w, 0x728, count + 1, (count + 1) * 12)
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
