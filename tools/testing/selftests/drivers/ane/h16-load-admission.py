#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""H16_LOAD admission and memory-cgroup charge check (ABI 3, H16G or H17).

Run it in a cgroup of its own, so the kernel-memory numbers are this
process's only:

  systemd-run --scope -p MemoryAccounting=yes --quiet \\
      ./h16-load-admission.py --hwx program.hwx

Steps, on one open file of the accel node:
  1. an oversized LOAD (ANE_H16_HWX_MAX + 1 bytes) fails with EINVAL;
  2. a valid LOAD succeeds, and the cgroup's kernel memory grows by at least
     the HWX size (the kernel's copy of the HWX is charged to the caller);
  3. a second LOAD whose pointers all fault fails with EBUSY, not EFAULT
     (admission is checked before any user memory is read);
  4. a second valid LOAD fails with EBUSY and leaves kernel memory unchanged;
  5. closing the file gives the charge back (reported only: a close during
     system sleep is deferred).
PASS needs 1-4. A kernel that copies before admission fails step 3 with
EFAULT; one that does not charge the copy fails step 2.
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
KINDS = (1, 2, 3, 4)
GET_CAPS = struct.Struct("<8I")
SECTION = struct.Struct("<IIQ")
LOAD = struct.Struct("<QQQIIII4Q4I")


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
            if (d / "memory.stat").exists():
                return d
    return None


def kernel_bytes(d):
    """The cgroup's charged kernel memory (memory.stat "kernel"), else memory.current."""
    for line in (d / "memory.stat").read_text().splitlines():
        key, _, value = line.partition(" ")
        if key == "kernel":
            return int(value)
    return int((d / "memory.current").read_text())


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
    print(f"device {path}, H{chip}; HWX {len(hwx)} bytes; cgroup {cg}")

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

    err = load(fd, hwx_ptr, HWX_MAX + 1, table_ptr)
    print(f"1. oversized LOAD: {name(err)} (want EINVAL)")
    if err != errno.EINVAL:
        result = KSFT_FAIL

    before = kernel_bytes(cg) if cg else None
    err = load(fd, hwx_ptr, len(hwx), table_ptr)
    after = kernel_bytes(cg) if cg else None
    if err:
        print(f"FAIL: 2. LOAD: {name(err)}")
        return KSFT_FAIL
    if cg:
        grew = after - before
        ok = grew >= len(hwx)
        print(f"2. LOAD: 0; cgroup kernel memory +{grew} bytes for a {len(hwx)}-byte HWX "
              f"({'charged' if ok else 'NOT charged'})")
        if not ok:
            result = KSFT_FAIL
    else:
        print("2. LOAD: 0; no cgroup v2 memory controller here, charge not checked")

    err = load(fd, 1, len(hwx), 1)
    print(f"3. second LOAD, faulting pointers: {name(err)} (want EBUSY)")
    if err != errno.EBUSY:
        result = KSFT_FAIL

    mid = kernel_bytes(cg) if cg else None
    err = load(fd, hwx_ptr, len(hwx), table_ptr)
    end = kernel_bytes(cg) if cg else None
    print(f"4. second valid LOAD: {name(err)} (want EBUSY)" +
          (f"; cgroup kernel memory {end - mid:+d} bytes" if cg else ""))
    if err != errno.EBUSY or (cg and end - mid >= len(hwx)):
        result = KSFT_FAIL

    os.close(fd)
    if cg:
        print(f"5. close: cgroup kernel memory {kernel_bytes(cg) - end:+d} bytes")
    print("PASS" if result == KSFT_PASS else "FAIL")
    return result


if __name__ == "__main__":
    sys.exit(main())
