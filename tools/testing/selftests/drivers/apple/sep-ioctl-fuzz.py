#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Robustness fuzzer for the Apple SEP character devices.

Drives /dev/sep-key (and the read-only and refused paths of /dev/sep-bio)
with malformed, truncated, unaligned and faulting requests, random bytes,
concurrent callers, cancels and signals. It never needs a finger: a call
that would wait for a touch is cancelled or interrupted, and nothing that
changes enrolled fingers (enrol, delete) is ever sent in a valid form.

Pass: every refusal is a clean errno, no call hangs, and the kernel log
gains no oops, BUG, WARNING, KASAN or kmemleak report. Needs root.
Exit 0 pass, 1 fail, 4 skip (kselftest).
"""

import ctypes
import errno
import mmap
import os
import random
import signal
import struct
import sys
import threading
import time

SEED = int(os.environ.get("SEP_FUZZ_SEED", "20261111"))
ROUNDS = int(os.environ.get("SEP_FUZZ_ROUNDS", "3000"))
KEY = "/dev/sep-key"
BIO = "/dev/sep-bio"

libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
libc.ioctl.restype = ctypes.c_int
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
PAGE = mmap.PAGESIZE

IOC_NONE, IOC_WRITE, IOC_READ = 0, 1, 2


def ioc(d, nr, size, magic=0xB1):
    return (d << 30) | (size << 16) | (magic << 8) | nr


# /dev/sep-key (drivers/soc/apple/key.rs).
K_INFO = ioc(IOC_READ, 0x20, 16)
K_CREATE = ioc(IOC_READ | IOC_WRITE, 0x21, 2124)
K_SIGN = ioc(IOC_READ | IOC_WRITE, 0x22, 2164)
K_PUBLIC = ioc(IOC_READ | IOC_WRITE, 0x23, 2124)
K_SELFTEST = ioc(IOC_READ | IOC_WRITE, 0x24, 2104)
K_CANCEL = ioc(IOC_NONE, 0x25, 0)
K_SEAL = ioc(IOC_READ | IOC_WRITE, 0x26, 2384)
K_UNSEAL = ioc(IOC_READ | IOC_WRITE, 0x27, 2384)
KEY_CMDS = [K_INFO, K_CREATE, K_SIGN, K_PUBLIC, K_SELFTEST, K_CANCEL, K_SEAL, K_UNSEAL]
HANDLE_MAX = 2048

# /dev/sep-bio (drivers/soc/apple/bio.rs): only harmless forms are sent.
B_GET_INFO = ioc(IOC_READ, 0x01, 32)
B_LIST = ioc(IOC_READ, 0x02, 4616)
B_ENROL_START = ioc(IOC_WRITE, 0x03, 144)
B_ENROL_POLL = ioc(IOC_READ, 0x04, 40)
B_VERIFY_START = ioc(IOC_WRITE, 0x05, 40)
B_VERIFY_POLL = ioc(IOC_READ, 0x06, 72)
B_CANCEL = ioc(IOC_NONE, 0x07, 0)
B_LIST_ENCLAVE = ioc(IOC_READ, 0x0B, 520)
B_LOCKOUT_STATE = ioc(IOC_READ, 0x0E, 16)

failures = []
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print(f"not ok {checks} - {what}", flush=True)


class Arena:
    """User memory with an inaccessible page right after it."""

    def __init__(self, pages=4):
        self.m = mmap.mmap(-1, (pages + 1) * PAGE)
        self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.m))
        self.size = pages * PAGE
        if libc.mprotect(self.base + self.size, PAGE, 0) != 0:
            raise OSError(ctypes.get_errno(), "mprotect")

    def put(self, off, data):
        self.m[off:off + len(data)] = data
        return self.base + off

    def get(self, off, n):
        return bytes(self.m[off:off + n])

    def end(self):
        return self.base + self.size


def call(fd, cmd, addr):
    rc = libc.ioctl(fd, cmd, addr)
    return 0 if rc == 0 else -ctypes.get_errno()


def size_of(cmd):
    return (cmd >> 16) & 0x3FFF


def handle(blob, flags=0):
    return b"SEPK" + bytes([1, flags, 0, 0]) + struct.pack("<I", len(blob)) + blob


def put_handle(req, off_len, off_handle, h):
    struct.pack_into("<I", req, off_len, len(h))
    req[off_handle:off_handle + len(h)] = h


def kmsg_mark():
    return len(os.popen("dmesg 2>/dev/null").read().splitlines())


def kmemleak_scan():
    """New unreferenced objects, when the kernel has kmemleak."""
    path = "/sys/kernel/debug/kmemleak"
    if not os.path.exists(path):
        return None
    with open(path, "w") as f:
        f.write("scan")
    with open(path) as f:
        return [l for l in f.read().splitlines() if "unreferenced object" in l]


BAD = ("Oops", "BUG:", "WARNING:", "KASAN", "kmemleak", "Call trace", "Unable to handle",
       "general protection", "refcount_t", "list_add corruption", "hung_task", "soft lockup")


def kmsg_new_problems(mark):
    lines = os.popen("dmesg 2>/dev/null").read().splitlines()[mark:]
    return [l for l in lines if any(b in l for b in BAD)]


def phase_info(fd):
    a = Arena()
    addr = a.put(0, bytes(16))
    check(call(fd, K_INFO, addr) == 0, "INFO")
    ver, flags, hmax, feats = struct.unpack("<IIII", a.get(0, 16))
    check(ver >= 3 and hmax == HANDLE_MAX, f"INFO version {ver} handle_max {hmax}")
    return ver, flags, feats


def phase_bad_pointers(fd):
    a = Arena()
    for cmd in KEY_CMDS:
        if cmd in (K_INFO, K_CANCEL):
            continue
        n = size_of(cmd)
        for addr, what in ((0, "NULL"), (a.end(), "guard page"), (a.end() - n + 1, "1 byte short"),
                           (a.end() - n + 64, "64 bytes short"), (0xFFFFFFFFFFFFF000, "kernel half"),
                           (1, "address 1")):
            rc = call(fd, cmd, addr)
            check(rc == -errno.EFAULT, f"cmd {cmd:#x} {what}: {rc}")
    check(call(fd, K_INFO, 0) == -errno.EFAULT, "INFO to NULL")
    check(call(fd, K_INFO, a.end() - 8) == -errno.EFAULT, "INFO across the guard page")


def phase_unknown(fd, rng):
    for cmd in KEY_CMDS:
        for bad in (cmd ^ (1 << 16), cmd ^ (1 << 30), (cmd & ~0xFF) | 0x7F):
            check(call(fd, bad, 0) == -errno.ENOTTY, f"unknown {bad:#x}")
    for _ in range(ROUNDS):
        cmd = rng.getrandbits(32)
        if cmd in KEY_CMDS:
            continue
        rc = call(fd, cmd, 0)
        check(rc == -errno.ENOTTY, f"random cmd {cmd:#x}: {rc}")


def create(fd, a, flags):
    req = bytearray(size_of(K_CREATE))
    struct.pack_into("<I", req, 0, flags)
    addr = a.put(0, req)
    rc = call(fd, K_CREATE, addr)
    if rc:
        return rc, None, None
    out = a.get(0, size_of(K_CREATE))
    n = struct.unpack_from("<I", out, 4)[0]
    # The handle is a 12-byte header and the enclave record; keep the record.
    return 0, out[76 + 12:76 + n], out[8:73]


def phase_create(fd, rng):
    a = Arena()
    for bit in range(32):
        flags = 1 << bit
        if flags in (1, 2):
            continue
        rc, _, _ = create(fd, a, flags)
        want = -errno.EOPNOTSUPP if flags in (4, 8) else -errno.EINVAL
        check(rc == want, f"CREATE flags {flags:#x}: {rc}")
    rc, _, _ = create(fd, a, 3)
    check(rc == -errno.EINVAL, f"CREATE two policies: {rc}")
    req = bytearray(size_of(K_CREATE))
    req[73] = 1
    check(call(fd, K_CREATE, a.put(5, req)) == -errno.EINVAL, "CREATE reserved")
    rc, plain, _ = create(fd, a, 0)
    check(rc == 0, f"CREATE plain key: {rc}")
    rc, bio, _ = create(fd, a, 2)
    if rc == -errno.ENODEV:
        bio = None  # Touch ID not started on this Mac.
    else:
        check(rc == 0, f"CREATE biometry-any key: {rc}")
    return plain, bio


def sign_req(h, digest=b"\x5a" * 32, flags=0):
    req = bytearray(size_of(K_SIGN))
    struct.pack_into("<I", req, 0, flags)
    req[8:40] = digest
    put_handle(req, 4, 116, h)
    return req


def seal_req(h, secret=b"", sealed=b""):
    req = bytearray(size_of(K_SEAL))
    put_handle(req, 4, 336, h)
    struct.pack_into("<II", req, 8, len(secret), len(sealed))
    req[16:16 + len(secret)] = secret
    req[80:80 + len(sealed)] = sealed
    return req


def phase_handles(fd, rng, plain, bio):
    a = Arena()
    good = handle(plain)
    for off in range(9):
        rc = call(fd, K_SIGN, a.put(off, sign_req(good, bytes(rng.getrandbits(8) for _ in range(32)))))
        check(rc == 0, f"SIGN plain key at offset {off}: {rc}")
    for n in (0, 1, 12, len(good) - 1, len(good) + 1, HANDLE_MAX + 1, 0xFFFFFFFF):
        req = sign_req(good)
        struct.pack_into("<I", req, 4, n)
        check(call(fd, K_SIGN, a.put(3, req)) == -errno.EINVAL, f"SIGN handle_len {n}")
    for flags in range(256):
        if flags in (0, 1, 3):
            continue
        h = bytearray(good)
        h[5] = flags
        check(call(fd, K_SIGN, a.put(0, sign_req(bytes(h)))) == -errno.EINVAL, f"SIGN handle flags {flags:#x}")
    check(call(fd, K_SIGN, a.put(0, sign_req(good, flags=1))) == -errno.EINVAL, "SIGN request flags")
    # Garbage, truncated and mutated records: refused, or (rarely) a valid
    # record of another key the enclave refuses.
    for i in range(ROUNDS):
        if i % 3 == 0:
            blob = bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 300)))
        elif i % 3 == 1:
            blob = plain[:rng.randint(1, len(plain))]
        else:
            b = bytearray(plain)
            for _ in range(rng.randint(1, 6)):
                b[rng.randrange(len(b))] = rng.getrandbits(8)
            blob = bytes(b)
        rc = call(fd, K_SIGN, a.put(rng.randrange(32), sign_req(handle(blob))))
        check(rc in (0, -errno.EINVAL, -errno.EKEYREJECTED, -errno.EIO),
              f"SIGN garbage record round {i}: {rc}")
    # PUBLIC on the same shapes.
    for i in range(ROUNDS // 4):
        req = bytearray(size_of(K_PUBLIC))
        blob = bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 200)))
        put_handle(req, 4, 76, handle(blob))
        rc = call(fd, K_PUBLIC, a.put(rng.randrange(32), req))
        check(rc in (0, -errno.EINVAL), f"PUBLIC garbage {i}: {rc}")
    # SEAL / UNSEAL shapes (no touch is ever asked for: all refused first).
    check(call(fd, K_SEAL, a.put(0, seal_req(good, b"x"))) == -errno.EINVAL, "SEAL plain key")
    if bio:
        bh = handle(bio, 3)
        for n in (0, 65, 0xFFFFFFFF):
            req = seal_req(bh, b"s" * 8)
            struct.pack_into("<I", req, 8, n)
            check(call(fd, K_SEAL, a.put(1, req)) == -errno.EINVAL, f"SEAL secret_len {n}")
        for sealed in (b"", b"\x04", b"\x05" + bytes(100), b"\x04" + bytes(80), bytes(256)):
            t0 = time.monotonic()
            rc = call(fd, K_UNSEAL, a.put(1, seal_req(bh, b"", sealed)))
            check(rc == -errno.EINVAL and time.monotonic() - t0 < 1,
                  f"UNSEAL malformed blob of {len(sealed)} bytes refused before any touch: {rc}")
        req = seal_req(bh, b"", b"\x04" + bytes(100))
        struct.pack_into("<I", req, 12, 257)
        check(call(fd, K_UNSEAL, a.put(1, req)) == -errno.EINVAL, "UNSEAL sealed_len 257")
        rc = call(fd, K_SEAL, a.put(0, seal_req(bh, os.urandom(32))))
        check(rc == 0, f"SEAL to the biometric key: {rc}")


def touch_call(fd, a, bio):
    """A touch-gated SIGN; returns (rc, seconds)."""
    t0 = time.monotonic()
    rc = call(fd, K_SIGN, a.put(0, sign_req(handle(bio, 3))))
    return rc, time.monotonic() - t0


def phase_cancel(fd, bio):
    if not bio:
        print("# no biometric key (Touch ID off): cancel/signal rows skipped")
        return
    a = Arena()
    out = {}

    def worker():
        f2 = os.open(KEY, os.O_RDWR)
        out["r"] = touch_call(f2, a, bio)
        os.close(f2)

    for delay in (0.0, 0.3, 2.0):
        t = threading.Thread(target=worker)
        t.start()
        time.sleep(delay)
        for _ in range(3):
            check(call(fd, K_CANCEL, 0) == 0, "CANCEL")
        t.join(70)
        check(not t.is_alive(), f"touch call after CANCEL ({delay} s) ended")
        rc, secs = out.get("r", (None, 99))
        # ECANCELED; or refused at once (EACCES: no password since boot or a
        # lockout); a cancel that came before the wait began lets it run to
        # ETIMEDOUT, which still must not hang.
        check(rc in (-errno.ECANCELED, -errno.EACCES, -errno.ETIMEDOUT, -errno.ENODEV),
              f"touch call after CANCEL: {rc} in {secs:.1f} s")
        if rc == -errno.ECANCELED:
            check(secs < delay + 6, f"CANCEL took {secs - delay:.1f} s")
    # A cancel with nothing waiting is a no-op.
    check(call(fd, K_CANCEL, 0) == 0, "idle CANCEL")


def phase_signals(bio):
    """A touch wait ends promptly when its caller gets a signal or dies."""
    if not bio:
        return
    for sig in (signal.SIGINT, signal.SIGKILL):
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            signal.signal(signal.SIGINT, lambda *_: None)
            fd = os.open(KEY, os.O_RDWR)
            a = Arena()
            os.write(w, b"go")
            rc, secs = touch_call(fd, a, bio)
            os.write(w, struct.pack("<id", rc, secs))
            os._exit(0)
        os.close(w)
        os.read(r, 2)
        time.sleep(1.0)
        t0 = time.monotonic()
        os.kill(pid, sig)
        deadline = t0 + 10
        while time.monotonic() < deadline:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                break
            time.sleep(0.05)
        else:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            done = 0
        secs = time.monotonic() - t0
        check(done != 0, f"touch call ended within 10 s of {sig.name} ({secs:.1f} s)")
        if sig == signal.SIGINT and done:
            data = os.read(r, 12)
            if len(data) == 12:
                rc, _ = struct.unpack("<id", data)
                check(rc in (-errno.EINTR, -errno.EACCES, -errno.ECANCELED),
                      f"touch call after SIGINT returned {rc}")
        os.close(r)


def phase_concurrent(plain, bio, rng):
    """Many callers at once: every answer is a clean result."""
    stop = time.monotonic() + 8
    bad = []

    def worker(seed):
        r = random.Random(seed)
        fd = os.open(KEY, os.O_RDWR)
        a = Arena()
        while time.monotonic() < stop:
            op = r.randrange(5)
            if op == 0:
                rc = call(fd, K_INFO, a.put(0, bytes(16)))
                ok = rc == 0
            elif op == 1:
                rc = call(fd, K_SIGN, a.put(r.randrange(16), sign_req(handle(plain))))
                ok = rc == 0
            elif op == 2:
                req = bytearray(size_of(K_PUBLIC))
                put_handle(req, 4, 76, handle(plain))
                rc = call(fd, K_PUBLIC, a.put(0, req))
                ok = rc == 0
            elif op == 3:
                rc = call(fd, K_CANCEL, 0)
                ok = rc == 0
            else:
                req = sign_req(handle(bytes(r.getrandbits(8) for _ in range(r.randint(1, 120)))))
                rc = call(fd, K_SIGN, a.put(0, req))
                ok = rc in (-errno.EINVAL, -errno.EKEYREJECTED)
            if not ok:
                bad.append((op, rc))
        os.close(fd)

    threads = [threading.Thread(target=worker, args=(rng.getrandbits(32),)) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
    check(all(not t.is_alive() for t in threads), "concurrent callers finished")
    check(not bad, f"concurrent callers: {len(bad)} bad answers, first {bad[:3]}")


def phase_bio(rng):
    """/dev/sep-bio: read-only calls and refused starts only."""
    try:
        fd = os.open(BIO, os.O_RDWR)
    except OSError as e:
        print(f"# {BIO}: {e.strerror}; skipped (fprintd may hold it)")
        return
    a = Arena()
    try:
        for cmd in (B_GET_INFO, B_LIST, B_ENROL_POLL, B_VERIFY_POLL, B_LIST_ENCLAVE, B_LOCKOUT_STATE):
            rc = call(fd, cmd, a.put(rng.randrange(16), bytes(size_of(cmd))))
            check(rc in (0, -errno.EBUSY, -errno.ENODEV, -errno.EIO, -errno.EPERM, -errno.ENOENT),
                  f"bio {cmd:#x}: {rc}")
            rc = call(fd, cmd, a.end() - 4)
            check(rc in (-errno.EFAULT, -errno.ENODEV, -errno.EBUSY, -errno.EIO, -errno.EPERM),
                  f"bio {cmd:#x} into the guard page: {rc}")
        for _ in range(ROUNDS // 10):
            req = bytearray(rng.getrandbits(8) for _ in range(40))
            req[0] |= 1  # flags != 0: always refused, no capture starts
            check(call(fd, B_VERIFY_START, a.put(rng.randrange(16), req)) == -errno.EINVAL,
                  "VERIFY_START with flags")
            req = bytearray(rng.getrandbits(8) for _ in range(144))
            struct.pack_into("<I", req, 8, rng.choice((0, 11, 12, 0xFFFFFFFF)))  # no such finger
            rc = call(fd, B_ENROL_START, a.put(rng.randrange(16), req))
            check(rc in (-errno.EINVAL, -errno.EROFS, -errno.ENODEV), f"ENROL_START bad finger: {rc}")
        rc = call(fd, B_CANCEL, 0)
        check(rc in (0, -errno.ENOENT), f"bio CANCEL idle: {rc}")
        for _ in range(200):
            cmd = rng.getrandbits(32)
            if (cmd >> 8) & 0xFF == 0xB1:
                continue
            check(call(fd, cmd, 0) == -errno.ENOTTY, f"bio random cmd {cmd:#x}")
    finally:
        os.close(fd)


def main():
    if os.geteuid() != 0:
        print("# needs root")
        return 4
    if not os.path.exists(KEY):
        print(f"# {KEY} missing")
        return 4
    rng = random.Random(SEED)
    print(f"# seed {SEED}, rounds {ROUNDS}")
    mark = kmsg_mark()
    leaks_before = kmemleak_scan()
    fd = os.open(KEY, os.O_RDWR)
    ver, flags, feats = phase_info(fd)
    print(f"# /dev/sep-key version {ver}, create flags {flags:#x}, features {feats:#x}")
    phase_bad_pointers(fd)
    phase_unknown(fd, rng)
    plain, bio = phase_create(fd, rng)
    if not plain:
        print("# CREATE failed: key store unavailable; later rows skipped")
    else:
        phase_handles(fd, rng, plain, bio)
        sel = bytearray(size_of(K_SELFTEST))
        struct.pack_into("<I", sel, 0, 20)
        sa = Arena()
        rc = call(fd, K_SELFTEST, sa.put(0, sel))
        check(rc in (-errno.EPERM, -errno.EOPNOTSUPP, -errno.EINVAL),
              f"self-test 20 refused on this kernel: {rc}")
        phase_concurrent(plain, bio, rng)
        phase_cancel(fd, bio)
        phase_signals(bio)
    phase_bio(rng)
    os.close(fd)
    leaks_after = kmemleak_scan()
    if leaks_after is not None:
        check(len(leaks_after) <= len(leaks_before or []), f"kmemleak: {len(leaks_after)} unreferenced objects")
    problems = kmsg_new_problems(mark)
    check(not problems, f"kernel log: {problems[:5]}")
    print(f"# {checks} checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
