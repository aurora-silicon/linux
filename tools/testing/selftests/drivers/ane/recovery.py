#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Destructive H13 hang/recovery test. Run only with exclusive device ownership."""
import argparse
import errno
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import signal
import struct
import sys
import time

# include/uapi/drm/ane_accel.h, Linux arm64's generic ioctl encoding.
CAPS = struct.Struct("<8I")
BO_INIT = struct.Struct("<IIQQ")
BO_FREE = struct.Struct("<II")
SUBMIT = struct.Struct("<QII32III")
ANEC = struct.Struct("<QIIQQII32I")
TILE = 16384
FIXTURE_HASHES = (
    "9a6a6a9a701ce207d4a8ea80ee1ca6d5ade076be2115b73a9aed6f4236aa56ab",
    "c6906b5be48e9bc12a76f3783753070cfa2514ce7f5539446deb15b6031e3514",
)


def request(number, layout):
    return (3 << 30) | (layout.size << 16) | (ord("d") << 8) | (0x40 + number)


class Fixture:
    """The two test fixtures use canonical H13 channels 4 (out), 5/6 (in).

    ANEC container layout and bootstrap NID encoding follow the public
    MIT libane implementation (omarchy-ane v0.5.7). This is deliberately a
    small test loader, not a replacement for the runtime's general binder.
    """
    def __init__(self, path):
        self.data = path.read_bytes()
        if len(self.data) < 4096:
            raise ValueError("truncated ANEC header")
        h = ANEC.unpack_from(self.data)
        self.size, self.td_size, self.td_count, self.tsk_size, kernel, self.inputs, outputs = h[:7]
        self.tiles = h[7:]
        if (not 4 <= self.td_size <= 0x40000 or self.td_size % 4 or
                not 1 <= self.td_count <= 0xffff or self.inputs not in (1, 2) or outputs != 1 or
                self.td_size * self.td_count > self.tsk_size or
                self.size != ((self.tsk_size + 15) & ~15) + kernel or
                len(self.data) - 4096 > self.size or self.size > 16 * 1024 * 1024 or
                self.size > self.tiles[0] * TILE or self.tsk_size >= self.tiles[0] * TILE or
                not self.tiles[4] or not self.tiles[5] or
                bool(self.tiles[6]) != (self.inputs == 2) or
                any(n for i, n in enumerate(self.tiles) if i not in (0, 4, 5, 6)) or
                sum(self.tiles) * TILE > 64 * 1024 * 1024):
            raise ValueError("unexpected canonical H13 ANEC layout")
        self.payload = self.data[4096:].ljust(self.size, b"\0")


class Program:
    def __init__(self, fixture, node):
        self.fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
        self.buffers = {}
        self.bootstrap = None
        self.fixture = fixture
        try:
            caps = bytearray(CAPS.size)
            fcntl.ioctl(self.fd, request(0, CAPS), caps)
            _, size, abi, chip, *_ = CAPS.unpack(caps)
            if size != CAPS.size or abi != 1 or chip != 13:
                raise RuntimeError("H13 ABI 1 required")
            for channel, tiles in enumerate(fixture.tiles):
                if tiles:
                    self.buffers[channel] = self.allocate(tiles * TILE)
            self.bootstrap = self.allocate((fixture.td_size + TILE - 1) & ~(TILE - 1))
            self.buffers[0][1][:fixture.size] = fixture.payload
            self.bootstrap[1][:fixture.td_size] = fixture.payload[:fixture.td_size]
            header, = struct.unpack_from("<I", self.bootstrap[1])
            struct.pack_into("<I", self.bootstrap[1], 0, (header & 0x0f00ffff) | (0x40 << 16))
        except BaseException:
            self.close()
            raise

    def allocate(self, size):
        args = bytearray(BO_INIT.pack(0, 0, size, 0))
        fcntl.ioctl(self.fd, request(1, BO_INIT), args)
        handle, _, _, offset = BO_INIT.unpack(args)
        try:
            mapping = mmap.mmap(self.fd, size, flags=mmap.MAP_SHARED,
                                prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset)
        except BaseException:
            fcntl.ioctl(self.fd, request(2, BO_FREE), BO_FREE.pack(handle, 0))
            raise
        return handle, mapping

    def execute(self):
        handles = [self.buffers[i][0] if i in self.buffers else 0 for i in range(32)]
        args = SUBMIT.pack(self.fixture.tsk_size, self.fixture.td_count,
                           self.fixture.td_size, *handles, self.bootstrap[0], 0)
        fcntl.ioctl(self.fd, request(3, SUBMIT), args)

    def close(self):
        buffers = list(self.buffers.values())
        if self.bootstrap:
            buffers.append(self.bootstrap)
        for handle, mapping in buffers:
            mapping.close()
            try:
                fcntl.ioctl(self.fd, request(2, BO_FREE), BO_FREE.pack(handle, 0))
            except OSError as error:
                # A failed reset must keep DMA backing; close the fd normally,
                # preserving the original test failure and the kernel's pin.
                print(f"BO_FREE failed: errno={error.errno}", file=sys.stderr, flush=True)
        self.buffers.clear()
        self.bootstrap = None
        os.close(self.fd)


def surface(values):
    result = bytearray(TILE)
    for index, value in enumerate(values):
        struct.pack_into("<e", result, index * 64, value)
    return bytes(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("good", type=Path, help="omarchy-ane v0.5.7 H13 add fixture")
    p.add_argument("hang", type=Path, help="ANEC from make-hang-fixture.py")
    p.add_argument("--device", type=Path, default=Path("/dev/accel/accel0"))
    p.add_argument("--check-fixtures", action="store_true", help="validate containers without device IO")
    args = p.parse_args()
    good_fixture, bad_fixture = Fixture(args.good), Fixture(args.hang)
    if tuple(hashlib.sha256(f.data).hexdigest() for f in (good_fixture, bad_fixture)) != FIXTURE_HASHES:
        raise ValueError("fixture SHA-256 differs from the public add or deterministic hang fixture")
    if (good_fixture.inputs != 2 or any(good_fixture.tiles[i] != 1 for i in (4, 5, 6)) or
            bad_fixture.inputs != 1):
        raise ValueError("expected H13 add and convolution fixtures")
    if args.check_fixtures:
        print("H13 add and hang ANEC containers validated; no device opened")
        return 0
    sysfs = Path("/sys/class/accel") / args.device.name / "device"
    if (sysfs / "driver").resolve().name != "ane":
        print("SKIP: H13 ane driver required")
        return 4
    boot = Path("/proc/sys/kernel/random/boot_id")
    refs = Path("/sys/module/ane/refcnt")
    before_boot, before_refs = boot.read_text(), refs.read_text()
    before_jobs = (sysfs / "ane_stats").read_text()
    # An outer controller watchdog must retain logs if an MMIO access blocks.
    signal.alarm(30)
    good = bad = None
    try:
        good = Program(good_fixture, args.device)
        bad = Program(bad_fixture, args.device)
        bad.buffers[5][1][:] = b"\x00\x3c" * (len(bad.buffers[5][1]) // 2)
        calls = 0
        def check_good():
            nonlocal calls
            inputs = [[(3 + (i * multiplier + calls) % 11) / 4 for i in range(64)]
                      for multiplier in (1, 3)]
            expected = surface([a + b for a, b in zip(*inputs)])
            for channel, values in zip((5, 6), inputs):
                good.buffers[channel][1][:] = surface(values)
            # Only the first fp16 of each plane is a logical output. Poison
            # those lanes and zero padding so stale success cannot pass.
            output = good.buffers[4][1]
            output[:] = bytes(TILE)
            for lane in range(64):
                struct.pack_into("<H", output, lane * 64, 0x7e00)
            good.execute()
            if output[:] != expected:
                raise RuntimeError("good output differs from independent fp16 add oracle")
            calls += 1
        check_good()
        for iteration in range(3):
            start = time.monotonic()
            error = 0
            try:
                bad.execute()
            except OSError as failure:
                error = failure.errno
            elapsed = time.monotonic() - start
            print(json.dumps({"iteration": iteration, "errno": error, "elapsed_s": elapsed}), flush=True)
            if error != errno.ETIMEDOUT or not 0.8 <= elapsed <= 5:
                raise RuntimeError("hang did not return a bounded ETIMEDOUT")
            if (sysfs / "wedged").read_text().strip() != "0":
                raise RuntimeError("automatic engine reset failed")
            # These original fds, GEM handles and mappings survive every reset.
            for _ in range(20):
                check_good()
    finally:
        if bad:
            bad.close()
        if good:
            good.close()
        signal.alarm(0)
    if boot.read_text() != before_boot:
        raise RuntimeError("boot identity changed")
    if refs.read_text() != before_refs:
        raise RuntimeError("module reference leak after closing both clients")
    print(json.dumps({"result": "PASS", "reboots": 0, "hangs": 3, "good_calls_after_reset": 60,
                      "good_sha256": hashlib.sha256(good_fixture.data).hexdigest(),
                      "hang_sha256": hashlib.sha256(bad_fixture.data).hexdigest(),
                      "stats_before": before_jobs,
                      "stats_after": (sysfs / "ane_stats").read_text()}), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
