#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stream NV12 1920x1080 frames through the Apple AVE MCTF video device.

Finds the device by its card name, streams N frames (a raw NV12 file, or
synthetic noise with a moving square), optionally writes the filtered
frames, and prints per-frame latency and a temporal-noise metric: the
standard deviation of the frame-to-frame luma difference in a static
region, for the input and for the output. Python 3 standard library only.
"""
import argparse
import ctypes as C
import fcntl
import mmap
import os
import select
import statistics
import sys
import time

W, H = 1920, 1080
FRAME = W * H * 3 // 2
CARD = b'Apple AVE MCTF'
LEVEL_ID = 0x00980900 + 0x1240	# V4L2_CID_USER_BASE + 0x1240
LEVEL_NAME = b'MCTF Level'

BUF_CAPTURE_MPLANE, BUF_OUTPUT_MPLANE = 9, 10
MEMORY_MMAP = 1
NV12 = int.from_bytes(b'NV12', 'little')
CTRL_FLAG_NEXT_CTRL = 0x80000000
BUF_FLAG_TIMESTAMP_COPY = 0x4000

# Static region (luma): away from the moving square
RX, RY, RS = 64, 64, 256
# Moving square: SQ x SQ, rows SQY.., SQV pixels per frame
SQ, SQY, SQV = 128, 600, 16


class Capability(C.Structure):
    _fields_ = [('driver', C.c_char * 16), ('card', C.c_char * 32),
                ('bus_info', C.c_char * 32), ('version', C.c_uint32),
                ('capabilities', C.c_uint32), ('device_caps', C.c_uint32),
                ('reserved', C.c_uint32 * 3)]


class PlanePix(C.Structure):
    _fields_ = [('sizeimage', C.c_uint32), ('bytesperline', C.c_uint32),
                ('reserved', C.c_uint16 * 6)]


class PixMp(C.Structure):
    _fields_ = [('width', C.c_uint32), ('height', C.c_uint32),
                ('pixelformat', C.c_uint32), ('field', C.c_uint32),
                ('colorspace', C.c_uint32), ('plane_fmt', PlanePix * 8),
                ('num_planes', C.c_uint8), ('flags', C.c_uint8),
                ('ycbcr_enc', C.c_uint8), ('quantization', C.c_uint8),
                ('xfer_func', C.c_uint8), ('reserved', C.c_uint8 * 7)]


class FmtUnion(C.Union):
    _fields_ = [('pix_mp', PixMp), ('raw', C.c_uint8 * 200),
                ('_align', C.c_void_p)]


class Format(C.Structure):
    _fields_ = [('type', C.c_uint32), ('fmt', FmtUnion)]


class RequestBuffers(C.Structure):
    _fields_ = [('count', C.c_uint32), ('type', C.c_uint32),
                ('memory', C.c_uint32), ('capabilities', C.c_uint32),
                ('flags', C.c_uint8), ('reserved', C.c_uint8 * 3)]


class PlaneM(C.Union):
    _fields_ = [('mem_offset', C.c_uint32), ('userptr', C.c_ulong),
                ('fd', C.c_int32)]


class Plane(C.Structure):
    _fields_ = [('bytesused', C.c_uint32), ('length', C.c_uint32),
                ('m', PlaneM), ('data_offset', C.c_uint32),
                ('reserved', C.c_uint32 * 11)]


class Timeval(C.Structure):
    _fields_ = [('tv_sec', C.c_long), ('tv_usec', C.c_long)]


class Timecode(C.Structure):
    _fields_ = [('type', C.c_uint32), ('flags', C.c_uint32),
                ('frames', C.c_uint8), ('seconds', C.c_uint8),
                ('minutes', C.c_uint8), ('hours', C.c_uint8),
                ('userbits', C.c_uint8 * 4)]


class BufferM(C.Union):
    _fields_ = [('offset', C.c_uint32), ('userptr', C.c_ulong),
                ('planes', C.POINTER(Plane)), ('fd', C.c_int32)]


class Buffer(C.Structure):
    _fields_ = [('index', C.c_uint32), ('type', C.c_uint32),
                ('bytesused', C.c_uint32), ('flags', C.c_uint32),
                ('field', C.c_uint32), ('timestamp', Timeval),
                ('timecode', Timecode), ('sequence', C.c_uint32),
                ('memory', C.c_uint32), ('m', BufferM),
                ('length', C.c_uint32), ('reserved2', C.c_uint32),
                ('request_fd', C.c_int32)]


class QueryCtrl(C.Structure):
    _fields_ = [('id', C.c_uint32), ('type', C.c_uint32),
                ('name', C.c_char * 32), ('minimum', C.c_int32),
                ('maximum', C.c_int32), ('step', C.c_int32),
                ('default_value', C.c_int32), ('flags', C.c_uint32),
                ('reserved', C.c_uint32 * 2)]


class Control(C.Structure):
    _fields_ = [('id', C.c_uint32), ('value', C.c_int32)]


def _ioc(d, nr, t):
    return (d << 30) | (C.sizeof(t) << 16) | (ord('V') << 8) | nr


QUERYCAP = _ioc(2, 0, Capability)
S_FMT = _ioc(3, 5, Format)
REQBUFS = _ioc(3, 8, RequestBuffers)
QUERYBUF = _ioc(3, 9, Buffer)
QBUF = _ioc(3, 15, Buffer)
DQBUF = _ioc(3, 17, Buffer)
STREAMON = _ioc(1, 18, C.c_int)
STREAMOFF = _ioc(1, 19, C.c_int)
G_CTRL = _ioc(3, 27, Control)
S_CTRL = _ioc(3, 28, Control)
QUERYCTRL = _ioc(3, 36, QueryCtrl)

assert C.sizeof(Format) == 208 and C.sizeof(Buffer) == 88 and C.sizeof(Plane) == 64


def find_device():
    for name in sorted(os.listdir('/dev')):
        if not name.startswith('video'):
            continue
        path = '/dev/' + name
        try:
            fd = os.open(path, os.O_RDWR)
        except OSError:
            continue
        cap = Capability()
        try:
            fcntl.ioctl(fd, QUERYCAP, cap)
            if cap.card == CARD:
                return fd, path, cap
        except OSError:
            pass
        os.close(fd)
    sys.exit('no "%s" video device (is the MCTF module loaded?)' % CARD.decode())


def level_control(fd):
    q = QueryCtrl(id=CTRL_FLAG_NEXT_CTRL)
    while True:
        try:
            fcntl.ioctl(fd, QUERYCTRL, q)
        except OSError:
            return LEVEL_ID, None
        if q.name == LEVEL_NAME:
            return q.id, q
        q.id |= CTRL_FLAG_NEXT_CTRL


def set_format(fd, typ):
    f = Format(type=typ)
    p = f.fmt.pix_mp
    p.width, p.height, p.pixelformat, p.num_planes = W, H, NV12, 1
    p.plane_fmt[0].bytesperline = W
    fcntl.ioctl(fd, S_FMT, f)
    if (p.width, p.height, p.pixelformat) != (W, H, NV12):
        sys.exit('format refused: %ux%u %08x' % (p.width, p.height, p.pixelformat))
    return p.plane_fmt[0].sizeimage, p.plane_fmt[0].bytesperline


def buffers(fd, typ, count):
    rb = RequestBuffers(count=count, type=typ, memory=MEMORY_MMAP)
    fcntl.ioctl(fd, REQBUFS, rb)
    maps = []
    for i in range(rb.count):
        planes = (Plane * 1)()
        b = Buffer(index=i, type=typ, memory=MEMORY_MMAP, length=1)
        b.m.planes = planes
        fcntl.ioctl(fd, QUERYBUF, b)
        maps.append(mmap.mmap(fd, planes[0].length, mmap.MAP_SHARED,
                              mmap.PROT_READ | mmap.PROT_WRITE,
                              offset=planes[0].m.mem_offset))
    return maps


def qbuf(fd, typ, index, used=0, ts_us=None):
    planes = (Plane * 1)()
    planes[0].bytesused = used
    b = Buffer(index=index, type=typ, memory=MEMORY_MMAP, length=1)
    b.m.planes = planes
    if ts_us is not None:
        b.timestamp.tv_sec, b.timestamp.tv_usec = divmod(ts_us, 1000000)
    fcntl.ioctl(fd, QBUF, b)


def dqbuf(fd, typ, timeout=12.0):
    planes = (Plane * 1)()
    b = Buffer(type=typ, memory=MEMORY_MMAP, length=1)
    b.m.planes = planes
    deadline = time.monotonic() + timeout
    while True:
        try:
            fcntl.ioctl(fd, DQBUF, b)
            return b, planes[0].bytesused
        except BlockingIOError:
            if time.monotonic() > deadline:
                raise TimeoutError('no %s buffer back in %.1f s' %
                                   ('CAPTURE' if typ == BUF_CAPTURE_MPLANE else 'OUTPUT',
                                    timeout))
            select.select([fd], [], [], 0.01)


class Pattern:
    """A static, noise-free test card: a horizontal luma ramp, eight
    vertical bars in the lower half and chroma ramps, so a tiled or
    reordered output is easy to recognise."""

    def __init__(self):
        ramp = bytes(16 + (219 * x) // (W - 1) for x in range(W))
        bars = b''.join(bytes([16 + 219 * (i % 2)]) * (W // 8) for i in range(8))
        y = ramp * (H // 2) + bars * (H - H // 2)
        u = [16 + (224 * x) // (W // 2 - 1) for x in range(W // 2)]
        rows = []
        for r in range(H // 2):
            v = 16 + (224 * r) // (H // 2 - 1)
            rows.append(bytes(b for x in range(W // 2) for b in (u[x], v)))
        self.data = y + b''.join(rows)

    def frame(self, n):
        return self.data


class Synth:
    """Flat grey with fixed-sigma noise and a moving white square."""

    def __init__(self, sigma):
        nd = statistics.NormalDist(0, sigma)
        luma = bytes(max(16, min(235, round(128 + nd.inv_cdf((k + 0.5) / 256))))
                     for k in range(256))
        nd = statistics.NormalDist(0, sigma / 2)
        chroma = bytes(max(16, min(240, round(128 + nd.inv_cdf((k + 0.5) / 256))))
                       for k in range(256))
        self.luma, self.chroma = luma, chroma

    def frame(self, n):
        y = bytearray(os.urandom(W * H).translate(self.luma))
        x0 = (n * SQV) % (W - SQ)
        row = b'\xeb' * SQ
        for r in range(SQY, SQY + SQ):
            y[r * W + x0:r * W + x0 + SQ] = row
        return bytes(y) + os.urandom(W * H // 2).translate(self.chroma)


def region(buf):
    return [bytes(buf[(RY + r) * W + RX:(RY + r) * W + RX + RS]) for r in range(RS)]


def diff_sd(a, b):
    if a is None:
        return None
    n = s = s2 = 0
    for ra, rb in zip(a, b):
        for x, y in zip(ra, rb):
            d = y - x
            s += d
            s2 += d * d
        n += len(ra)
    mean = s / n
    return (s2 / n - mean * mean) ** 0.5


def square_mean(buf, n):
    x0 = (n * SQV) % (W - SQ)
    vals = [buf[(SQY + SQ // 2) * W + x0 + i] for i in range(16, SQ - 16)]
    return sum(vals) / len(vals)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('-n', '--frames', type=int, default=60)
    ap.add_argument('-i', '--input', help='raw NV12 1920x1080 file (looped)')
    ap.add_argument('-o', '--output', help='write the filtered NV12 frames here')
    ap.add_argument('-l', '--level', type=int, help='MCTF Level (0..31)')
    ap.add_argument('--sigma', type=float, default=8.0, help='synthetic luma noise')
    ap.add_argument('--pattern', action='store_true', help='static noise-free test card')
    ap.add_argument('--depth', type=int, default=2, help='frames in flight')
    ap.add_argument('--fps', type=float, default=30.0, help='timestamps only')
    ap.add_argument('--warmup', type=int, default=6, help='frames left out of the summary')
    args = ap.parse_args()

    fd, path, cap = find_device()
    print('device %s: driver %s card %s' % (path, cap.driver.decode(), cap.card.decode()))
    cid, qc = level_control(fd)
    if qc:
        print('control "%s" id %#x range %d..%d default %d' %
              (qc.name.decode(), qc.id, qc.minimum, qc.maximum, qc.default_value))
    else:
        print('control "MCTF Level" not listed; using id %#x' % cid)
    if args.level is not None:
        fcntl.ioctl(fd, S_CTRL, Control(id=cid, value=args.level))
    c = Control(id=cid)
    fcntl.ioctl(fd, G_CTRL, c)
    print('MCTF Level %d' % c.value)

    out_size, stride = set_format(fd, BUF_OUTPUT_MPLANE)
    cap_size, _ = set_format(fd, BUF_CAPTURE_MPLANE)
    print('NV12 %ux%u stride %u, sizeimage OUTPUT %u CAPTURE %u' %
          (W, H, stride, out_size, cap_size))
    omaps = buffers(fd, BUF_OUTPUT_MPLANE, max(args.depth + 1, 2))
    cmaps = buffers(fd, BUF_CAPTURE_MPLANE, max(args.depth + 1, 2))
    print('%d OUTPUT and %d CAPTURE buffers' % (len(omaps), len(cmaps)))
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

    src = open(args.input, 'rb') if args.input else None
    synth = None if src else (Pattern() if args.pattern else Synth(args.sigma))
    sink = open(args.output, 'wb') if args.output else None

    for i in range(len(cmaps)):
        qbuf(fd, BUF_CAPTURE_MPLANE, i)
    fcntl.ioctl(fd, STREAMON, C.c_int(BUF_OUTPUT_MPLANE))
    fcntl.ioctl(fd, STREAMON, C.c_int(BUF_CAPTURE_MPLANE))

    free_out = list(range(len(omaps)))
    sent = {}		# timestamp (us) -> (frame number, send time, input region, square)
    prev_in = prev_out = None
    rows = []
    blank = same = ts_bad = 0
    n_sent = n_done = 0
    t0 = time.monotonic()

    def collect():
        nonlocal prev_in, prev_out, n_done, blank, same, ts_bad
        b, used = dqbuf(fd, BUF_CAPTURE_MPLANE)
        now = time.monotonic()
        ts = b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec
        if ts not in sent:
            ts_bad += 1
            print('frame ?: CAPTURE timestamp %d matches no OUTPUT frame' % ts)
            qbuf(fd, BUF_CAPTURE_MPLANE, b.index)
            return
        n, t_sent, in_reg, in_sq = sent.pop(ts)
        m = cmaps[b.index]
        out = m[:FRAME]
        out_reg = region(out)
        if not any(out[:W * 16]) and not any(out[W * H:W * H + W * 8]):
            blank += 1
        if out_reg == in_reg:
            same += 1
        sd_in, sd_out = diff_sd(prev_in, in_reg), diff_sd(prev_out, out_reg)
        rows.append((n, (now - t_sent) * 1e3, sd_in, sd_out))
        print('frame %3d: seq %3u bytes %u latency %6.2f ms  diff sd in %s out %s  square in %.0f out %.0f%s' %
              (n, b.sequence, used, (now - t_sent) * 1e3,
               '%5.2f' % sd_in if sd_in is not None else '  -  ',
               '%5.2f' % sd_out if sd_out is not None else '  -  ',
               in_sq, square_mean(out, n), '  ERROR' if b.flags & 0x40 else ''))
        if sink:
            sink.write(out)
        prev_in, prev_out = in_reg, out_reg
        n_done += 1
        qbuf(fd, BUF_CAPTURE_MPLANE, b.index)
        ob, _ = dqbuf(fd, BUF_OUTPUT_MPLANE)
        free_out.append(ob.index)

    try:
        while n_sent < args.frames:
            if src:
                data = src.read(FRAME)
                if len(data) < FRAME:
                    src.seek(0)
                    data = src.read(FRAME)
                    if len(data) < FRAME:
                        sys.exit('input file holds no complete frame')
            else:
                data = synth.frame(n_sent)
            idx = free_out.pop(0)
            omaps[idx][:FRAME] = data
            ts = int(n_sent * 1e6 / args.fps) + 1
            sent[ts] = (n_sent, time.monotonic(), region(data), square_mean(data, n_sent))
            qbuf(fd, BUF_OUTPUT_MPLANE, idx, used=out_size, ts_us=ts)
            n_sent += 1
            while len(sent) >= args.depth or not free_out:
                collect()
        while sent:
            collect()
    finally:
        fcntl.ioctl(fd, STREAMOFF, C.c_int(BUF_OUTPUT_MPLANE))
        fcntl.ioctl(fd, STREAMOFF, C.c_int(BUF_CAPTURE_MPLANE))
        if sink:
            sink.close()

    total = time.monotonic() - t0
    body = [r for r in rows if r[0] >= args.warmup and r[2] is not None]
    lat = sorted(r[1] for r in rows)
    print('---')
    print('%d frames in %.2f s (%.1f fps); latency median %.2f ms, p90 %.2f ms, max %.2f ms' %
          (n_done, total, n_done / total, lat[len(lat) // 2], lat[9 * len(lat) // 10], lat[-1]))
    if body:
        sin = statistics.mean(r[2] for r in body)
        sout = statistics.mean(r[3] for r in body)
        print('static-region frame-to-frame luma diff sd (frames %d..): input %.2f output %.2f (output/input %.2f)' %
              (args.warmup, sin, sout, sout / sin if sin else float('nan')))
    print('timestamps copied: %s; blank outputs: %d; outputs identical to input in the region: %d' %
          ('yes' if not ts_bad else 'NO (%d mismatched)' % ts_bad, blank, same))
    return 1 if ts_bad or blank or n_done != args.frames else 0


if __name__ == '__main__':
    sys.exit(main())
