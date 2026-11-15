#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Load the pinned H16G add-128 HWX through the ABI 3 accel node and run it.

The program is loaded once and executed twice. Before each run the output
buffer is poisoned, and after each run it must equal expected.bin exactly.
Requires a kernel booted with the H16 program stage; skips otherwise.
"""

import argparse
import ctypes
import fcntl
import glob
import hashlib
import importlib.util
import os
import pathlib
import struct
import sys

KSFT_PASS, KSFT_FAIL, KSFT_SKIP = 0, 1, 4

DRM_COMMAND_BASE = 0x40
DRM_ANE_ABI_V3 = 3
DRM_ANE_CHIP_H16 = 16
SECTIONS = (
    (1, "generic.bin"),
    (2, "operation.bin"),
    (3, "procedure.bin"),
    (4, "text-properties.bin"),
)
GET_CAPS = struct.Struct("<8I")
SECTION = struct.Struct("<IIQ")
LOAD = struct.Struct("<QQQIIII4Q4I")
BUFFER = struct.Struct("<QQ")
EXEC = struct.Struct("<QII")
POISON = 0xff


def ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("d") << 8) | (DRM_COMMAND_BASE + nr)


IOCTL_GET_CAPS = ioc(3, 0x00, GET_CAPS.size)
IOCTL_H16_LOAD = ioc(3, 0x07, LOAD.size)
IOCTL_H16_EXEC = ioc(1, 0x08, EXEC.size)


def load_module(name, file):
    path = pathlib.Path(__file__).resolve().parent / file
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find_device(explicit, errors):
    candidates = [explicit] if explicit else sorted(glob.glob("/dev/accel/accel*"))
    for path in candidates:
        try:
            fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        except OSError as e:
            errors.append(f"open({path}): {e.strerror} (errno {e.errno})")
            continue
        caps = bytearray(GET_CAPS.size)
        try:
            fcntl.ioctl(fd, IOCTL_GET_CAPS, caps)
        except OSError:
            # Not an ANE node (another accel driver); not a failure of ours.
            os.close(fd)
            continue
        _, _, abi, chip, _, _, _, _ = GET_CAPS.unpack(caps)
        if abi == DRM_ANE_ABI_V3 and chip == DRM_ANE_CHIP_H16:
            return path, fd
        os.close(fd)
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hwx", type=pathlib.Path, required=True,
                        help="pinned add-128 HWX from prepare_alu")
    parser.add_argument("--sections", type=pathlib.Path,
                        help="directory with generic.bin, operation.bin, procedure.bin "
                             "and text-properties.bin for this HWX; default: build them "
                             "with h16-sections.py")
    parser.add_argument("--device", help="accel node; default: the first ABI 3 H16 node")
    parser.add_argument("--runs", type=int, default=2)
    args = parser.parse_args()

    fixture = load_module("h16_fixture", "h16-fixture.py")
    hwx = args.hwx.read_bytes()
    if len(hwx) != fixture.HWX_SIZE or hashlib.sha256(hwx).hexdigest() != fixture.HWX_SHA256:
        print("FAIL: HWX is not the pinned add-128 fixture")
        return KSFT_FAIL
    data = fixture.buffers()
    if args.sections:
        sections = [(kind, (args.sections / name).read_bytes()) for kind, name in SECTIONS]
    else:
        built, ids, refs, _ = load_module("h16_sections", "h16-sections.py").build(hwx)
        sections = list(zip((kind for kind, _ in SECTIONS), built))
        print("sections built: buffer IDs", ids, "BAR refs", refs)

    errors = []
    path, fd = find_device(args.device, errors)
    if fd is None and errors:
        # An accel node exists but cannot be opened: a driver failure, not a skip.
        for error in errors:
            print(f"FAIL: {error}")
        return KSFT_FAIL
    if fd is None:
        print("SKIP: no ABI 3 H16 accel node (program stage not reached)")
        return KSFT_SKIP
    print(f"device {path}")

    keep = []
    hwx_buf = ctypes.create_string_buffer(hwx, len(hwx))
    table = bytearray()
    for kind, blob in sections:
        buf = ctypes.create_string_buffer(blob, len(blob))
        keep.append(buf)
        table += SECTION.pack(kind, len(blob), ctypes.addressof(buf))
    table_buf = ctypes.create_string_buffer(bytes(table), len(table))
    load = bytearray(LOAD.pack(ctypes.addressof(hwx_buf), len(hwx),
                               ctypes.addressof(table_buf), len(sections), 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0))
    try:
        fcntl.ioctl(fd, IOCTL_H16_LOAD, load)
    except OSError as e:
        print(f"FAIL: H16_LOAD: {os.strerror(e.errno)}")
        return KSFT_FAIL
    fields = LOAD.unpack(load)
    count = fields[5]
    sizes = fields[7:11][:count]
    directions = fields[11:15][:count]
    print(f"loaded: {count} surfaces, sizes {list(sizes)}, directions {list(directions)}")
    if count != 3 or sorted(directions) != [1, 1, 2] or set(sizes) != {32768}:
        print("FAIL: add-128 must have two 32768-byte inputs and one output")
        return KSFT_FAIL

    inputs = iter((data["a.bin"], data["b.bin"]))
    buffers = []
    for direction in directions:
        payload = next(inputs) if direction == 1 else bytes([POISON]) * 32768
        buffers.append(ctypes.create_string_buffer(payload, 32768))
    output = buffers[directions.index(2)]
    expected = data["expected.bin"]
    table = b"".join(BUFFER.pack(ctypes.addressof(b), 32768) for b in buffers)
    table_buf = ctypes.create_string_buffer(table, len(table))

    result = KSFT_PASS
    for run in range(1, args.runs + 1):
        ctypes.memset(output, POISON, 32768)
        try:
            fcntl.ioctl(fd, IOCTL_H16_EXEC, EXEC.pack(ctypes.addressof(table_buf), count, 0))
        except OSError as e:
            print(f"run {run}: FAIL: H16_EXEC: {os.strerror(e.errno)}")
            result = KSFT_FAIL
            break
        got = output.raw
        values = struct.unpack("<16384H", got)
        want = struct.unpack("<16384H", expected)
        exact = sum(1 for x, y in zip(values, want) if x == y)
        digest = hashlib.sha256(got).hexdigest()
        print(f"run {run}: {exact}/16384 fp16 values exact, output SHA-256 {digest}")
        if got != expected:
            result = KSFT_FAIL
            break
    os.close(fd)
    print("PASS" if result == KSFT_PASS else "FAIL")
    return result


if __name__ == "__main__":
    sys.exit(main())
