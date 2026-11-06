#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the opt-in Aura PV-KVM discovery and bounded-run ABI."""

import errno
import fcntl
import os
import struct
import sys


IOC_NRBITS = 8
IOC_TYPEBITS = 8
IOC_SIZEBITS = 14
IOC_DIRBITS = 2
IOC_NRSHIFT = 0
IOC_TYPESHIFT = IOC_NRSHIFT + IOC_NRBITS
IOC_SIZESHIFT = IOC_TYPESHIFT + IOC_TYPEBITS
IOC_DIRSHIFT = IOC_SIZESHIFT + IOC_SIZEBITS
IOC_READ = 2
IOC_WRITE = 1


def ioctl_number(direction, nr, size):
    return ((direction << IOC_DIRSHIFT) | (ord("P") << IOC_TYPESHIFT) |
            (nr << IOC_NRSHIFT) | (size << IOC_SIZESHIFT))


DISCOVER = ioctl_number(IOC_READ, 0, 88)
RUN = ioctl_number(IOC_READ | IOC_WRITE, 1, 120)


def main():
    try:
        fd = os.open("/dev/aura-pv", os.O_RDWR | os.O_CLOEXEC)
    except FileNotFoundError:
        print("SKIP: /dev/aura-pv is not present (Aura PV is opt-in)")
        return 4
    try:
        caps = bytearray(88)
        fcntl.ioctl(fd, DISCOVER, caps, True)
        (
            major,
            minor,
            size,
            layout,
            features,
            max_bytes,
            alignment,
            vmid_bits,
            ipa_bits,
            granule,
            host_el,
            _,
            _,
        ) = struct.unpack("<4I9Q", caps)
        if (major, minor, size, layout) != (0, 1, 88, 0x43504130):
            raise AssertionError("unexpected Aura PV capabilities")
        if not features & 1 or max_bytes != 4096 or alignment != 4096:
            raise AssertionError("missing bounded shared-page capability")
        if granule != 14 or host_el != 2 or not vmid_bits or not ipa_bits:
            raise AssertionError("invalid stage-2 capability description")

        words = (1, 2, 3)
        program = (*words, 0, 0, 0, 0, 0)
        req = struct.pack("<QQ8QQQQQq", 1, 10, *program, 3, 0, 0, 0, 0)
        result = bytearray(req)
        fcntl.ioctl(fd, RUN, result, True)
        fields = struct.unpack("<QQ8QQQQQq", result)
        if (
            fields[10] != 3
            or fields[11] != 16
            or fields[12] != 1
            or fields[13] != 0
            or fields[14] != 0
        ):
            raise AssertionError("Aura PV bounded-run result mismatch")
        print("Aura PV discovery and bounded-run ABI: PASS")
        return 0
    except OSError as exc:
        if exc.errno in (errno.ENODEV, errno.ENOSYS, errno.EPROTO):
            print(f"SKIP: Aura PV endpoint unavailable ({exc})")
            return 4
        raise
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
