#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""H16_LOAD admission and memory-cgroup charge check (ABI 3, H16G or H17).

Run it in a cgroup of its own, so the memory numbers are this process's
only:

  systemd-run --scope -p MemoryAccounting=yes --quiet \\
      ./h16-load-admission.py --hwx program.hwx

Steps, on one open file of the accel node:
  1. an oversized LOAD (ANE_H16_HWX_MAX + 1 bytes) fails with EINVAL;
  2. a 64 MiB LOAD of zero bytes fails validation with ENOEXEC, and the
     cgroup's peak memory rose by at least 48 MiB while the kernel held its
     copy: the copy is charged to the caller;
  3. a valid LOAD of --hwx succeeds;
  4. a second LOAD whose pointers all fault fails with EBUSY, not EFAULT;
  5. a second 64 MiB LOAD fails with EBUSY, and the peak rises by less than
     16 MiB: admission refuses it before anything is copied.

The 64 MiB are read from a private anonymous mapping that was never
written, so userspace reads them from the shared zero page and is not
charged for them; only the kernel's copy is. A small HWX cannot show the
charge: memory cgroups batch charges per CPU, which is also why step 2
asks for 48 MiB rather than all 64. The steps read the cgroup's
memory.peak, reset for this file descriptor where the kernel allows it.

PASS needs every step. A kernel that does not charge the copy fails step 2,
and one that copies before admission fails step 4 with EFAULT.
"""

import argparse
import ctypes
import errno
import fcntl
import glob
import importlib.util
import os
import pathlib
import struct
import sys

KSFT_PASS, KSFT_FAIL, KSFT_SKIP = 0, 1, 4
DRM_COMMAND_BASE = 0x40
DRM_ANE_ABI_V3 = 3
CHIPS = (15, 16, 17)
HWX_MAX = 256 * 1024 * 1024 + 1024 * 1024
IMPORT = 64 * 1024 * 1024
# Per-CPU charge batches can absorb part of the copy; nothing else is near.
CHARGED, UNCHARGED = IMPORT * 3 // 4, IMPORT // 4
KINDS = (1, 2, 3, 4)
GET_CAPS = struct.Struct("<8I")
SECTION = struct.Struct("<IIQ")
LOAD = struct.Struct("<QQQIIII4Q4I")
PROT_READ, MAP_PRIVATE, MAP_ANONYMOUS = 0x1, 0x02, 0x20


def ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("d") << 8) | (DRM_COMMAND_BASE + nr)


IOCTL_GET_CAPS = ioc(3, 0x00, GET_CAPS.size)
IOCTL_H16_LOAD = ioc(3, 0x07, LOAD.size)


def find_device(explicit):
    for path in [explicit] if explicit else sorted(glob.glob("/dev/accel/accel*")):
        try:
            fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            continue
        caps = bytearray(GET_CAPS.size)
        try:
            fcntl.ioctl(fd, IOCTL_GET_CAPS, caps)
        except OSError:
            os.close(fd)
            continue
        _, _, abi, chip, _, _, _, _ = GET_CAPS.unpack(caps)
        if abi == DRM_ANE_ABI_V3 and chip in CHIPS:
            return path, fd, chip
        os.close(fd)
    return None, None, None


def cgroup_dir():
    for line in pathlib.Path("/proc/self/cgroup").read_text().splitlines():
        if line.startswith("0::"):
            d = pathlib.Path("/sys/fs/cgroup") / line[3:].lstrip("/")
            if (d / "memory.current").exists() and (d / "memory.peak").exists():
                return d
    return None


class Peak:
    """The cgroup's memory.peak, reset per file descriptor where supported."""

    def __init__(self, cg):
        self.cg = cg
        try:
            self.fd = os.open(cg / "memory.peak", os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            self.fd = os.open(cg / "memory.peak", os.O_RDONLY | os.O_CLOEXEC)

    def read(self):
        os.lseek(self.fd, 0, os.SEEK_SET)
        return int(os.read(self.fd, 64))

    def growth(self, call):
        """Run call(); return its result and how far the peak rose above usage before it."""
        try:
            os.write(self.fd, b"reset\n")
            floor = None
        except OSError:
            floor = self.read()
        current = int((self.cg / "memory.current").read_text())
        result = call()
        peak = self.read()
        if floor is not None and floor >= current + UNCHARGED:
            return result, None
        return result, peak - current

    def close(self):
        os.close(self.fd)


def load(fd, hwx_ptr, hwx_size, table_ptr):
    args = bytearray(LOAD.pack(hwx_ptr, hwx_size, table_ptr, len(KINDS), 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0))
    try:
        fcntl.ioctl(fd, IOCTL_H16_LOAD, args)
    except OSError as e:
        return e.errno
    return 0


def name(err):
    return "0" if not err else errno.errorcode.get(err, str(err))


def mib(n):
    return f"{n / (1024 * 1024):.1f} MiB"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hwx", type=pathlib.Path, required=True)
    parser.add_argument("--device")
    args = parser.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location("h16_sections", here / "h16-sections.py")
    sections = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sections)
    hwx = args.hwx.read_bytes()
    built = sections.build(hwx)[0]

    path, fd, chip = find_device(args.device)
    if fd is None:
        print("SKIP: no ABI 3 accel node (program stage not reached)")
        return KSFT_SKIP
    cg = cgroup_dir()
    peak = Peak(cg) if cg else None
    print(f"device {path}, H{chip}; HWX {len(hwx)} bytes; cgroup {cg}")

    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    zeros = libc.mmap(None, IMPORT, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
    if zeros is None or zeros == ctypes.c_void_p(-1).value:
        print(f"FAIL: mmap: {os.strerror(ctypes.get_errno())}")
        return KSFT_FAIL

    keep = []
    table = bytearray()
    for kind, blob in zip(KINDS, built):
        buf = ctypes.create_string_buffer(blob, len(blob))
        keep.append(buf)
        table += SECTION.pack(kind, len(blob), ctypes.addressof(buf))
    table_buf = ctypes.create_string_buffer(bytes(table), len(table))
    hwx_buf = ctypes.create_string_buffer(hwx, len(hwx))
    hwx_ptr, table_ptr = ctypes.addressof(hwx_buf), ctypes.addressof(table_buf)
    result = KSFT_PASS

    def import_zeros():
        return load(fd, zeros, IMPORT, table_ptr)

    def measured(call):
        return peak.growth(call) if peak else (call(), None)

    err = load(fd, hwx_ptr, HWX_MAX + 1, table_ptr)
    print(f"1. oversized LOAD: {name(err)} (want EINVAL)")
    if err != errno.EINVAL:
        result = KSFT_FAIL

    err, grew = measured(import_zeros)
    if grew is None:
        print(f"2. 64 MiB LOAD: {name(err)} (want ENOEXEC); peak not measurable here")
    else:
        charged = grew >= CHARGED
        print(f"2. 64 MiB LOAD: {name(err)} (want ENOEXEC); cgroup peak +{mib(grew)} "
              f"({'charged' if charged else 'NOT charged'})")
        if not charged:
            result = KSFT_FAIL
    if err != errno.ENOEXEC:
        result = KSFT_FAIL

    err = load(fd, hwx_ptr, len(hwx), table_ptr)
    print(f"3. LOAD of {args.hwx.name}: {name(err)}")
    if err:
        print("FAIL")
        return KSFT_FAIL

    err = load(fd, 1, len(hwx), 1)
    print(f"4. second LOAD, faulting pointers: {name(err)} (want EBUSY)")
    if err != errno.EBUSY:
        result = KSFT_FAIL

    err, grew = measured(import_zeros)
    print(f"5. second 64 MiB LOAD: {name(err)} (want EBUSY)" +
          (f"; cgroup peak +{mib(grew)}" if grew is not None else ""))
    if err != errno.EBUSY or (grew is not None and grew >= UNCHARGED):
        result = KSFT_FAIL

    os.close(fd)
    if peak:
        peak.close()
    print("PASS" if result == KSFT_PASS else "FAIL")
    return result


if __name__ == "__main__":
    sys.exit(main())
