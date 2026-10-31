#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Destructive H13 hang/recovery test. Run only with exclusive device ownership."""
import argparse
import ctypes
import ctypes.util
import errno
import hashlib
import json
from pathlib import Path
import signal
import struct
import sys
import time


def bind(library, name, result, *arguments):
    fn = getattr(library, name)
    fn.restype, fn.argtypes = result, arguments
    return fn


def surface(values):
    result = bytearray(16384)
    for index, value in enumerate(values):
        struct.pack_into("<e", result, index * 64, value)
    return bytes(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("good", type=Path, help="omarchy-ane v0.5.7 H13 add fixture")
    p.add_argument("hang", type=Path, help="ANEC from make-hang-fixture.py")
    p.add_argument("--libane", default=ctypes.util.find_library("ane"))
    p.add_argument("--sysfs", type=Path, default=Path("/sys/class/accel/accel0/device"))
    args = p.parse_args()
    if not args.libane:
        print("SKIP: libane unavailable")
        return 4
    if (args.sysfs / "driver").resolve().name != "ane":
        print("SKIP: H13 ane driver required")
        return 4
    lib = ctypes.CDLL(args.libane, use_errno=True)
    ptr, u32, u64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    init = bind(lib, "__ane_init", ptr, ctypes.c_char_p, ctypes.c_int)
    free = bind(lib, "__ane_free", None, ptr)
    execute = bind(lib, "ane_exec", ctypes.c_int, ptr)
    src_size = bind(lib, "__ane_src_size", u64, ptr, u32)
    dst_size = bind(lib, "__ane_dst_size", u64, ptr, u32)
    send = bind(lib, "__ane_send", None, ptr, ptr, u32)
    read = bind(lib, "__ane_read", None, ptr, ptr, u32)
    boot = Path("/proc/sys/kernel/random/boot_id")
    refs = Path("/sys/module/ane/refcnt")
    before_boot, before_refs = boot.read_text(), refs.read_text()
    before_jobs = (args.sysfs / "ane_stats").read_text()
    # An outer controller watchdog should retain logs if an MMIO access blocks.
    signal.alarm(30)
    good = bad = None
    try:
        good = init(bytes(args.good), 0)
        bad = init(bytes(args.hang), 0)
        if not good or not bad:
            raise RuntimeError("fixture load failed before hang injection")
        if src_size(good, 0) != 16384 or src_size(good, 1) != 16384 or dst_size(good, 0) != 16384:
            raise RuntimeError("expected the packaged H13 add fixture")
        inputs = [[(3 + (i * multiplier) % 11) / 4 for i in range(64)] for multiplier in (1, 3)]
        expected = surface([a + b for a, b in zip(*inputs)])
        for index, values in enumerate(inputs):
            send(good, ctypes.create_string_buffer(surface(values)), index)
        bad_size = src_size(bad, 0)
        if not 0 < bad_size <= 16 * 1024 * 1024:
            raise RuntimeError("unexpected hang input size")
        send(bad, ctypes.create_string_buffer(bad_size), 0)
        output = ctypes.create_string_buffer(16384)
        def check_good():
            if execute(good) != 0:
                raise RuntimeError(f"good submit failed: errno={ctypes.get_errno()}")
            read(good, output, 0)
            if output.raw != expected:
                raise RuntimeError("good output differs from independent fp16 add oracle")
        check_good()
        for iteration in range(3):
            start = time.monotonic()
            ctypes.set_errno(0)
            result = execute(bad)
            error = ctypes.get_errno()
            elapsed = time.monotonic() - start
            print(json.dumps({"iteration": iteration, "result": result, "errno": error,
                              "elapsed_s": elapsed}), flush=True)
            if result != -1 or error != errno.ETIMEDOUT or not 0.8 <= elapsed <= 5:
                raise RuntimeError("hang did not return a bounded ETIMEDOUT")
            if (args.sysfs / "wedged").read_text().strip() != "0":
                raise RuntimeError("automatic engine reset failed")
            # This live client's original fd, GEM handles and mappings survive
            # all three resets; no reopen or program reload can hide corruption.
            for _ in range(20):
                check_good()
    finally:
        if bad:
            free(bad)
        if good:
            free(good)
        signal.alarm(0)
    if boot.read_text() != before_boot:
        raise RuntimeError("boot identity changed")
    if refs.read_text() != before_refs:
        raise RuntimeError("module reference leak after closing both clients")
    print(json.dumps({"result": "PASS", "reboots": 0, "hangs": 3, "good_calls_after_reset": 60,
                      "good_sha256": hashlib.sha256(args.good.read_bytes()).hexdigest(),
                      "hang_sha256": hashlib.sha256(args.hang.read_bytes()).hexdigest(),
                      "stats_before": before_jobs,
                      "stats_after": (args.sysfs / "ane_stats").read_text()}), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
