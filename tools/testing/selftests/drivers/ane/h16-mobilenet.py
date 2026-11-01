#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a linked H16G MobileNetV1 224 program through the ABI 3 accel node.

The package directory holds linked/program-0.hwx (one program, 28 tasks,
DRAM scratch on BAR slot 3), refs/input_NN.bin (fp16 input surfaces),
refs/output_NN.bin (the CPU fp16-chain output surfaces) and refs/top1.json.
The program is loaded once; each image is one EXECUTE with a poisoned
output. The output surface is the 1001-class logit map as 1024 channels of
7x7 fp16 (row 64 bytes, plane 448 bytes); top-1 is the channel with the
largest 7x7 mean. PASS needs every image's top-1 to match top1.json.
The device output is not expected to equal the CPU chain bit for bit; the
exact-value count and largest difference are reported for information.
"""

import argparse
import ctypes
import fcntl
import importlib.util
import json
import os
import pathlib
import statistics
import struct
import sys
import time

KSFT_PASS, KSFT_FAIL, KSFT_SKIP = 0, 1, 4
CLASSES, ROW, PLANE = 1001, 64, 448
POISON = 0xff


def load_module(name, file):
    path = pathlib.Path(__file__).resolve().parent / file
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def top1(surface):
    best, best_mean = 0, None
    for c in range(CLASSES):
        total = 0.0
        for h in range(7):
            total += sum(struct.unpack_from("<7e", surface, c * PLANE + h * ROW))
        if best_mean is None or total > best_mean:
            best, best_mean = c, total
    return best


def compare(got, want):
    a = struct.unpack(f"<{len(got) // 2}e", got)
    b = struct.unpack(f"<{len(want) // 2}e", want)
    exact, worst = 0, 0.0
    for c in range(CLASSES):
        for h in range(7):
            for w in range(7):
                k = (c * PLANE + h * ROW) // 2 + w
                exact += a[k] == b[k]
                worst = max(worst, abs(a[k] - b[k]))
    return exact, CLASSES * 49, worst


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", type=pathlib.Path, required=True)
    parser.add_argument("--device", help="accel node; default: the first ABI 3 H16 node")
    parser.add_argument("--images", type=int, default=16)
    parser.add_argument("--timing-runs", type=int, default=20,
                        help="extra EXECUTEs of image 0 for host latency")
    args = parser.parse_args()

    run = load_module("h16_run", "h16-run.py")
    hwx = (args.package / "linked/program-0.hwx").read_bytes()
    refs = args.package / "refs"
    want_top1 = json.loads((refs / "top1.json").read_text())["top1"][:args.images]
    try:
        built, ids, bar_refs, tasks = load_module("h16_sections", "h16-sections.py").build(hwx)
    except ValueError as e:
        print(f"FAIL: sections: {e}")
        return KSFT_FAIL
    print("sections built: buffer IDs", ids, "BAR refs", bar_refs, "tasks", tasks)

    errors = []
    path, fd = run.find_device(args.device, errors)
    if fd is None and errors:
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
    for (kind, _), blob in zip(run.SECTIONS, built):
        buf = ctypes.create_string_buffer(blob, len(blob))
        keep.append(buf)
        table += run.SECTION.pack(kind, len(blob), ctypes.addressof(buf))
    table_buf = ctypes.create_string_buffer(bytes(table), len(table))
    load = bytearray(run.LOAD.pack(ctypes.addressof(hwx_buf), len(hwx),
                                   ctypes.addressof(table_buf), len(built), 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0))
    try:
        fcntl.ioctl(fd, run.IOCTL_H16_LOAD, load)
    except OSError as e:
        print(f"FAIL: H16_LOAD: {os.strerror(e.errno)} (errno {e.errno})")
        return KSFT_FAIL
    fields = run.LOAD.unpack(load)
    count = fields[5]
    sizes = list(fields[7:11][:count])
    directions = list(fields[11:15][:count])
    print(f"loaded: {count} surfaces, sizes {sizes}, directions {directions}")
    if count != 2 or directions != [1, 2]:
        print("FAIL: MobileNet must have one input and one output surface")
        return KSFT_FAIL
    in_size, out_size = sizes
    buffers = [ctypes.create_string_buffer(in_size), ctypes.create_string_buffer(out_size)]
    exec_table = b"".join(run.BUFFER.pack(ctypes.addressof(b), len(b)) for b in buffers)
    exec_buf = ctypes.create_string_buffer(exec_table, len(exec_table))

    def execute(image):
        data = (refs / f"input_{image:02d}.bin").read_bytes()
        if len(data) != in_size:
            raise ValueError(f"input_{image:02d}.bin is {len(data)} bytes, surface {in_size}")
        ctypes.memmove(buffers[0], data, in_size)
        ctypes.memset(buffers[1], POISON, out_size)
        start = time.perf_counter()
        fcntl.ioctl(fd, run.IOCTL_H16_EXEC, run.EXEC.pack(ctypes.addressof(exec_buf), count, 0))
        return time.perf_counter() - start

    result, matched = KSFT_PASS, 0
    for image in range(args.images):
        try:
            seconds = execute(image)
        except (OSError, ValueError) as e:
            print(f"img{image}: FAIL: {e}")
            result = KSFT_FAIL
            break
        got = buffers[1].raw
        if got == bytes([POISON]) * out_size:
            print(f"img{image}: FAIL: output untouched")
            result = KSFT_FAIL
            break
        t = top1(got)
        exact, total, worst = compare(got, (refs / f"output_{image:02d}.bin").read_bytes())
        ok = t == want_top1[image]
        matched += ok
        print(f"img{image}: ANE top1={t} ref={want_top1[image]} {'OK' if ok else 'DIFF'}; "
              f"{exact}/{total} logits equal the CPU chain, max |diff| {worst:.4g}; "
              f"{seconds * 1e3:.3f} ms")
    if result == KSFT_PASS:
        times = []
        for _ in range(args.timing_runs):
            try:
                times.append(execute(0))
            except OSError as e:
                print(f"timing: FAIL: {e}")
                result = KSFT_FAIL
                break
        if times:
            times.sort()
            print(f"host EXECUTE latency over {len(times)} runs: p50 "
                  f"{statistics.median(times) * 1e3:.3f} ms, "
                  f"p90 {times[int(len(times) * 0.9) - 1] * 1e3:.3f} ms")
        if matched != args.images:
            result = KSFT_FAIL
    os.close(fd)
    print(f"top-1 {matched}/{args.images}")
    print("PASS" if result == KSFT_PASS else "FAIL")
    return result


if __name__ == "__main__":
    sys.exit(main())
