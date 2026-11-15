#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Load one H16G or H17 HWX through the ABI 3 accel node and run cases.

The program is loaded once. Each --case names the input files, one per
input surface in the order H16_LOAD reports, and optionally the expected
output. Before every run the output buffer is poisoned. With --expect the
output must match within --max-ulp on every element the expected file
covers, except elements it sets to 0xffff (padding lanes). With --out-dir each case's last output is saved as output_NN.bin.

  ane-run.py --hwx program.hwx --case in.bin:expected.bin --runs 20
  ane-run.py --hwx program.hwx --case a.bin,b.bin --out-dir out

Requires a kernel whose ANE reached the program stage; skips otherwise.
"""

import argparse
import ctypes
import fcntl
import glob
import importlib.util
import os
import pathlib
import statistics
import struct
import sys
import time

KSFT_PASS, KSFT_FAIL, KSFT_SKIP = 0, 1, 4

DRM_COMMAND_BASE = 0x40
DRM_ANE_ABI_V3 = 3
CHIPS = (16, 17)
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


def find_device(explicit):
    candidates = [explicit] if explicit else sorted(glob.glob("/dev/accel/accel*"))
    for path in candidates:
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


def ulp_key(h):
    """Map an fp16 bit pattern onto a line where adjacent values differ by 1."""
    return 0x8000 - (h & 0x7fff) if h & 0x8000 else 0x8000 + h


def compare(got, want):
    """Exact count, compared count and the largest ulp distance.

    Expected elements equal to 0xffff, a NaN pattern, mark padding lanes of
    the surface layout and are not compared.
    """
    n = min(len(got), len(want)) // 2
    pairs = [(x, y) for x, y in zip(struct.unpack(f"<{n}H", got[:2 * n]),
                                    struct.unpack(f"<{n}H", want[:2 * n])) if y != 0xffff]
    exact = sum(1 for x, y in pairs if x == y)
    worst = max((abs(ulp_key(x) - ulp_key(y)) for x, y in pairs), default=0)
    return exact, len(pairs), worst


def parse_case(text):
    inputs, _, expect = text.partition(":")
    return [pathlib.Path(p) for p in inputs.split(",") if p], \
        pathlib.Path(expect) if expect else None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hwx", type=pathlib.Path, required=True)
    parser.add_argument("--sections", type=pathlib.Path,
                        help="directory with the four section files; default: build "
                             "them with h16-sections.py")
    parser.add_argument("--case", action="append", default=[],
                        help="IN[,IN...][:EXPECTED], repeatable")
    parser.add_argument("--runs", type=int, default=1, help="executions per case")
    parser.add_argument("--max-ulp", type=int, default=0)
    parser.add_argument("--out-dir", type=pathlib.Path)
    parser.add_argument("--device", help="accel node; default: the first ABI 3 node")
    args = parser.parse_args()

    hwx = args.hwx.read_bytes()
    if args.sections:
        sections = [(kind, (args.sections / name).read_bytes()) for kind, name in SECTIONS]
    else:
        built, ids, refs, tasks = load_module("h16_sections", "h16-sections.py").build(hwx)
        sections = list(zip((kind for kind, _ in SECTIONS), built))
        print(f"sections built: buffer IDs {ids}, BAR refs {refs}, tasks {tasks}")

    path, fd, chip = find_device(args.device)
    if fd is None:
        print("SKIP: no ABI 3 accel node (program stage not reached)")
        return KSFT_SKIP
    print(f"device {path}, H{chip}")

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
    start = time.monotonic()
    try:
        fcntl.ioctl(fd, IOCTL_H16_LOAD, load)
    except OSError as e:
        print(f"FAIL: H16_LOAD: {os.strerror(e.errno)}")
        return KSFT_FAIL
    fields = LOAD.unpack(load)
    count = fields[5]
    sizes = fields[7:11][:count]
    directions = fields[11:15][:count]
    print(f"loaded in {(time.monotonic() - start) * 1e3:.1f} ms: {count} surfaces, "
          f"sizes {list(sizes)}, directions {list(directions)}")
    buffers = [ctypes.create_string_buffer(size) for size in sizes]
    output = directions.index(2)
    table = b"".join(BUFFER.pack(ctypes.addressof(b), s) for b, s in zip(buffers, sizes))
    table_buf = ctypes.create_string_buffer(table, len(table))
    exec_args = EXEC.pack(ctypes.addressof(table_buf), count, 0)

    result = KSFT_PASS
    times = []
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
    for n, case in enumerate(args.case or [""]):
        inputs, expect = parse_case(case)
        slots = [i for i, d in enumerate(directions) if d == 1]
        if inputs and len(inputs) != len(slots):
            print(f"case {n}: FAIL: {len(inputs)} input files for {len(slots)} inputs")
            result = KSFT_FAIL
            break
        for i, f in zip(slots, inputs):
            data = f.read_bytes()[:sizes[i]]
            ctypes.memset(buffers[i], 0, sizes[i])
            ctypes.memmove(buffers[i], data, len(data))
        want = expect.read_bytes() if expect else None
        for run in range(args.runs):
            ctypes.memset(buffers[output], POISON, sizes[output])
            start = time.monotonic_ns()
            try:
                fcntl.ioctl(fd, IOCTL_H16_EXEC, exec_args)
            except OSError as e:
                print(f"case {n} run {run}: FAIL: H16_EXEC: {os.strerror(e.errno)}")
                result = KSFT_FAIL
                break
            times.append(time.monotonic_ns() - start)
            if want is not None and run in (0, args.runs - 1):
                exact, total, worst = compare(buffers[output].raw, want)
                print(f"case {n} run {run}: {exact}/{total} exact, max {worst} ulp")
                if worst > args.max_ulp:
                    result = KSFT_FAIL
        if result != KSFT_PASS:
            break
        if args.out_dir:
            (args.out_dir / f"output_{n:02d}.bin").write_bytes(buffers[output].raw)
    if times:
        times.sort()
        p = lambda q: times[min(len(times) - 1, int(q * len(times)))] / 1e3
        print(f"host latency over {len(times)} runs: p50 {p(0.5):.1f} us, "
              f"p90 {p(0.9):.1f} us, p99 {p(0.99):.1f} us, "
              f"mean {statistics.mean(times) / 1e3:.1f} us")
    os.close(fd)
    print("PASS" if result == KSFT_PASS else "FAIL")
    return result


if __name__ == "__main__":
    sys.exit(main())
