#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Generate the MSR23 scaler's polyphase filter table (apple-scaler-msr23.c).

The engine filters each axis with 9 taps and picks one of 8 coefficient sets
("bins") by the step, src/dst in Q19: a step uses the first bin whose ratio is
at least the step. Each set is one symmetric kernel h(x), sampled at 9 source
pixels around the output position with 32 sub-pixel phases per pixel.

Design
  Downscaling bins (ratio s > 1, s = 8/7, 4/3, 8/5, 2, 8/3, 4):
    h(x) = sinc(x / s) * kaiser(x; R = 3.5 s, beta = 6.5)
    A low-pass at the output's Nyquist frequency (cutoff 1/s of the source
    rate), Kaiser-windowed over three and a half lobes of the scaled sinc.
  Upscaling bin (steps below 1):
    h(x) = sinc(x) * kaiser(x; R = 31/16, beta = 5)
    An interpolating 4-tap kernel: h(0) = 1, h(+-1) = 0, nothing at |x| >= 2.
  The 1:1 bin passes the source through: the centre tap is 1.0 in every
  phase, and its extra word is 1.0.
  Window: kaiser(x; R, beta) = I0(beta * sqrt(1 - (x/R)^2)) / I0(beta) for
  |x| < R, else 0.

Sampling and quantisation
  Phase f = p/32 (p = 0..16) places the output f source pixels right of tap 4,
  so tap k sits at x = k - 4 - f. Each phase is normalised to unit gain and
  rounded to Q12 (4096 = 1.0); the rounding residual goes to the largest tap,
  so every phase sums to exactly 4096.

Storage (16 words per tap, index tap * 16 + phase)
  Phases 1..15 are stored as they are, in columns 1..15. The kernel is
  symmetric, so phases 17..31 are the mirror images of 15..1 and are not
  stored. Column 0 holds two phases: its taps 4..8 are f = 0 (taps 0..3 are
  their mirror), and its taps 0..3 hold taps 1..4 of f = 1/2. The per-bin
  extra word is tap 0 of f = 1/2 (taps 5..8 are the mirror of 1..4).

Against the coefficient set captured from the engine's own jobs (pass it
with --compare), every word is within 5/4096 and every phase's absolute
difference sums to at most 14/4096: under 0.9 of an 8-bit code value per
axis, so a scaled 8-bit sample differs by at most 2.

Usage: msr23_filters.py              print the C initializer
       msr23_filters.py --check F    exit 1 unless source file F holds exactly this table
       msr23_filters.py --compare F  per-bin differences against the table in F
"""
import math
import re
import sys

PHASES = 32
TAPS = 9
ONE = 4096
# Bin upper bounds, src/dst in Q19 (the 1:1 bin is exactly 1 << 19).
BINS = [
    ("up", (1 << 19) - 1),
    ("one", 1 << 19),
    ("down", 8 * (1 << 19) // 7),
    ("down", 4 * (1 << 19) // 3),
    ("down", 8 * (1 << 19) // 5),
    ("down", 2 << 19),
    ("down", 8 * (1 << 19) // 3),
    ("down", 4 << 19),
]


def i0(x):
    """Modified Bessel function of the first kind, order 0 (power series)."""
    s, t, k = 1.0, 1.0, 1
    while t > 1e-17 * s:
        t *= (x / (2 * k)) ** 2
        s += t
        k += 1
    return s


def sinc(x):
    return 1.0 if x == 0 else math.sin(math.pi * x) / (math.pi * x)


def kaiser(x, r, beta):
    if abs(x) >= r:
        return 0.0
    return i0(beta * math.sqrt(1.0 - (x / r) ** 2)) / i0(beta)


def kernel(kind, ratio):
    s = ratio / (1 << 19)
    if kind == "up":
        return lambda x: sinc(x) * kaiser(x, 31 / 16, 5.0)
    return lambda x: sinc(x / s) * kaiser(x, 3.5 * s, 6.5)


def phase(h, f):
    w = [h(k - 4 - f) for k in range(TAPS)]
    total = sum(w)
    q = [w[k] / total * ONE for k in range(TAPS)]
    r = [int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5)) for v in q]
    big = max(range(TAPS), key=lambda k: q[k])
    r[big] += ONE - sum(r)
    return r


def bin_words(kind, ratio):
    if kind == "one":
        taps = [ONE if k == 4 else 0 for k in range(TAPS) for _ in range(16)]
        return ONE, taps
    h = kernel(kind, ratio)
    cols = {p: phase(h, p / PHASES) for p in range(17)}
    taps = [0] * (TAPS * 16)
    for k in range(TAPS):
        for p in range(1, 16):
            taps[k * 16 + p] = cols[p][k]
    for k in range(4, TAPS):
        taps[k * 16] = cols[0][k]
    for k in range(4):
        taps[k * 16] = cols[16][k + 1]
    return cols[16][0], taps


def table():
    return [(ratio,) + bin_words(kind, ratio) for kind, ratio in BINS]


def emit():
    out = []
    for ratio, extra, taps in table():
        out.append(f"\t{{ .ratio = 0x{ratio:x}, .extra = {extra}, .taps = {{")
        for i in range(0, len(taps), 12):
            out.append("\t\t" + ", ".join(str(v) for v in taps[i:i + 12]) + ",")
        out.append("\t} },")
    return "\n".join(out)


def parse(path):
    src = open(path).read()
    bins = re.findall(r"\.ratio = (0x[0-9a-f]+), \.extra = (-?\d+), \.taps = \{(.*?)\}", src, re.S)
    return [(int(r, 16), int(e), [int(v) for v in re.findall(r"-?\d+", t)]) for r, e, t in bins]


def main():
    if len(sys.argv) == 3 and sys.argv[1] in ("--check", "--compare"):
        ref = parse(sys.argv[2])
        worst = 0
        for (r0, e0, t0), (r1, e1, t1) in zip(table(), ref):
            d = max([abs(a - b) for a, b in zip(t0, t1)] + [abs(e0 - e1)])
            n = sum(a != b for a, b in zip(t0, t1)) + (e0 != e1)
            print(f"bin 0x{r0:x}: ratio {'ok' if r0 == r1 else 'DIFF'}, {n} words differ, max {d}")
            worst = max(worst, d)
        if sys.argv[1] == "--check":
            sys.exit(0 if worst == 0 else 1)
        return
    print(emit())


if __name__ == "__main__":
    main()
