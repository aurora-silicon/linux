#!/usr/bin/env python3
"""Synthetic kiwi captures for the replay harness, same line format as the real ones.

kiwi-synth.py OUT.frames MODE
  idle      : three 2 s cycles (720 frames, 120 Hz), references +658 +902 -445 -126 with
              per-row lean and noisier top rows. Expect no contact ever.
  stuckedge : idle plus cells (52..53, 0..1) held 400 counts low from frame 200 on,
              mimicking the 16:12 phantom: baseline first, then the top edge sticks.
  stuckmid  : cells (25..27, 40..42) held 400 counts low from frame 200 on: the hold
              rule case away from the edge ring.
  wander    : a breathing blob (plus or minus 200 counts) that hops one cell every
              90 frames from frame 200 on: lively and moving, so only the 45 s
              hold rule can judge it.
  flat2s    : a perfectly flat 400-count blob at the centre for 2 s from frame 200:
              the M5 menu-open phantom shape (static, no ramp, no breathing).
  tap       : idle plus a wobbling fingertip near the panel centre for 24 frames.
  tapedge   : same tap but near the top edge, so the edge guard is exercised by a
              finger-like blob that must STILL be reported after confirmation.
  presshold : a real 39 s breathing touch, a lift, then a genuine new press at the
              same cell 60 frames later held to the end: the new press must inherit
              nothing and stay reported.
  bootfinger: a flat 5x5 blob 400 counts low at the centre from frame 0 through
              frame 400: a finger already resting on the glass at input open.
              The baseline must not absorb it: the blob must be reported.
  bootfinger2 : the same boot finger, a lift at 400, 200 idle frames, then a tap
              at the same spot (600) and a tap on a different row (648).
  bootfinger3 : the same but the second tap is on the SAME rows as the boot
              finger: measures whether the absorbed bake scars those rows.
  breathelong : a stationary tent blob ramping to 400 counts over 600 frames
              (the breathing starts after the ramp: +-39 sine, period 1 s)
              at the centre to the end of an 8000-frame run:
              lively by breathing, zero drift, so only the hold rule ends it.
              The suite runs it with --expect bound: the hold rule must drop
              it at about frame born+5400 and keep every contact bounded.
  breatheretap : the breathelong blob, lifted at 6600 for 2.5 s, then the
              same finger taps again at 6900: after a real lift the map must
              free the cells so the tap registers (2 contacts, both bounded).
  bootfinger4 : the same boot finger and lift, then a stuckmid-style flat
              400-count blob on the SAME rows from frame 500: at the absorbed
              bake the blob cancels the scar and is never seen (shallow or
              equal-depth touches on those rows are invisible all session).
"""
import math
import sys
import random

HDR, COLS, ROWS = 61, 82, 54
CELLS = COLS * ROWS
FRAME = 61 + 2 * CELLS + 8944  # 17861, like the real reports

path, mode = sys.argv[1], sys.argv[2]
rng = random.Random(11)
nframes = 8000 if mode in ("breathelong", "holdrebirth") else 7200 if mode in ("presshold", "breatheretap") else 1400 if mode == "lifteretap" else 720
refs = [658, 902, -445, -126]  # whole-image shifts, one per 0.5 s
idle = [[2500 + rng.randint(-30, 30) for _ in range(COLS)] for _ in range(ROWS)]
for r in range(ROWS - 4, ROWS):  # top rows noisier
    for c in range(COLS):
        idle[r][c] = 2500 + rng.randint(-60, 60)


def shift_for(f, r):
    if f % 60:  # the real chip sends one reference frame, then 59 ordinary ones
        return 0
    k = (f // 60) % 4
    base = refs[k]
    row_lean = (r - 26) * (2 if base > 0 else -2)
    if r >= ROWS - 4:  # top-edge rows move hundreds more on reference frames
        row_lean += (300 if base > 0 else -300)
    return base + row_lean


def cell_frame(f):
    drift = 3 * f // (130 * 120)
    img = [[idle[r][c] + rng.randint(-5, 5) + drift + shift_for(f, r) for c in range(COLS)]
           for r in range(ROWS)]
    if mode in ("stuckedge",) and f >= 200:
        for r in (52, 53):
            for c in (0, 1):
                img[r][c] -= 400
    if mode in ("stuckmid",) and f >= 200:
        for r in (25, 26, 27):
            for c in (40, 41, 42):
                img[r][c] -= 400
    if mode in ("tap", "tapedge") and 100 <= f < 124:
        cr, cc = (4, 2) if mode == "tapedge" else (26, 41)
        jr, jc = rng.randint(-1, 1), rng.randint(-1, 1)
        ramp = min(f - 100, 123 - f, 5) * 160
        breath = rng.randint(-70, 70)
        for r in range(cr - 2, cr + 3):
            for c in range(cc - 2, cc + 3):
                if 0 <= r < ROWS and 0 <= c < COLS:
                    d = max(abs(r - cr), abs(c - cc))
                    img[r][c] -= (1000 if d == 0 else 600) * ramp // 800
                    img[r][c] -= 10 * abs(jr) + 10 * abs(jc) + breath
    if mode == "wander" and f >= 200:
        cr, cc = 26 + (f // 90) % 2, 41 + ((f // 90) // 2) % 2
        breath = rng.randint(-200, 200)
        for r in (cr - 1, cr, cr + 1):
            for c in (cc - 1, cc, cc + 1):
                if 0 <= r < ROWS and 0 <= c < COLS:
                    d = max(abs(r - cr), abs(c - cc))
                    img[r][c] -= 400 if d == 0 else 250
                    img[r][c] -= breath // 3
    if mode == "presshold" and 200 <= f < 4900:
        cr, cc = 26, 41
        breath = rng.randint(-200, 200)
        for r in (cr - 1, cr, cr + 1):
            for c in (cc - 1, cc, cc + 1):
                if 0 <= r < ROWS and 0 <= c < COLS:
                    d = max(abs(r - cr), abs(c - cc))
                    img[r][c] -= 400 if d == 0 else 250
                    img[r][c] -= breath // 3
    if mode == "presshold" and f >= 4960:
        cr, cc = 26, 41
        jr, jc = rng.randint(-1, 1), rng.randint(-1, 1)
        breath = rng.randint(-200, 200)
        for r in range(cr - 2, cr + 3):
            for c in range(cc - 2, cc + 3):
                if 0 <= r < ROWS and 0 <= c < COLS:
                    d = max(abs(r - cr), abs(c - cc))
                    img[r][c] -= (1000 if d == 0 else 600) if d <= 2 else 0
                    img[r][c] -= 10 * abs(jr) + 10 * abs(jc) + breath
    if mode == "flat2s" and 200 <= f < 440:
        for r in (25, 26, 27):
            for c in (40, 41, 42):
                img[r][c] -= 400
    if mode == "holdrebirth" and f >= 600:
        # the original always-lit capture: a flat 5x5 from frame 600, breathing
        # +-80. At the parent this is 4 contacts; the two after the drop must die.
        base = 400 + int(80 * math.sin(2 * math.pi * f / 120))
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= base
    if mode == "lifteretap":
        # a finger, a real lift, quiet past the lift window, then the same
        # cells again. The second touch must register.
        if 200 <= f < 800 or 1100 <= f < 1180:
            base = 400 + int(39 * math.sin(2 * math.pi * f / 120))
            for r in range(24, 29):
                for c in range(39, 44):
                    img[r][c] -= base * (3 - abs(r - 26)) * (3 - abs(c - 41)) // 9
    if mode == "breathelong" and f >= 1:
        base = 600 + int(39 * math.sin(2 * math.pi * f / 120))
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= base * (3 - abs(r - 26)) * (3 - abs(c - 41)) // 9
    if mode == "breatheretap" and f >= 1 and not (6600 <= f < 6900):
        ramp = min(f, 600)
        base = ramp * 400 // 600 + (int(39 * math.sin(2 * math.pi * f / 120)) if f > 600 else 0)
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= base * (3 - abs(r - 26)) * (3 - abs(c - 41)) // 9
    if mode == "bootfinger" and f < 400:
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= 400
    if mode == "bootfinger4" and f < 400:
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= 400
    if mode == "bootfinger4" and f >= 500:
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= 400
    if mode in ("bootfinger2", "bootfinger3") and f < 400:
        for r in range(24, 29):
            for c in range(39, 44):
                img[r][c] -= 400
    if mode in ("bootfinger2", "bootfinger3") and 600 <= f < 624:
        jr, jc = rng.randint(-1, 1), rng.randint(-1, 1)
        ramp = min(f - 600, 623 - f, 5) * 160
        breath = rng.randint(-70, 70)
        for r in range(24, 29):
            for c in range(39, 44):
                d = max(abs(r - 26), abs(c - 41))
                img[r][c] -= (1000 if d == 0 else 600) * ramp // 800
                img[r][c] -= 10 * abs(jr) + 10 * abs(jc) + breath
    if mode == "bootfinger2" and 648 <= f < 672:
        jr, jc = rng.randint(-1, 1), rng.randint(-1, 1)
        ramp = min(f - 648, 671 - f, 5) * 160
        breath = rng.randint(-70, 70)
        for r in range(10, 15):
            for c in range(60, 65):
                d = max(abs(r - 12), abs(c - 62))
                img[r][c] -= (1000 if d == 0 else 600) * ramp // 800
                img[r][c] -= 10 * abs(jr) + 10 * abs(jc) + breath
    if mode == "bootfinger3" and 648 <= f < 672:
        jr, jc = rng.randint(-1, 1), rng.randint(-1, 1)
        ramp = min(f - 648, 671 - f, 5) * 160
        breath = rng.randint(-70, 70)
        for r in range(24, 29):
            for c in range(60, 65):
                d = max(abs(r - 26), abs(c - 62))
                img[r][c] -= (1000 if d == 0 else 600) * ramp // 800
                img[r][c] -= 10 * abs(jr) + 10 * abs(jc) + breath
    return img


with open(path, "w") as fh:
    for f in range(nframes):
        raw = bytearray(FRAME)
        raw[0:7] = bytes([0x5A, 0x01, 0x02, 0x00, 0x20, 0x18, 0x00])
        raw[7:11] = (f * 8333).to_bytes(4, "little")
        raw[15:17] = (f & 0xFFFF).to_bytes(2, "little")
        raw[23] = f & 0xFF
        raw[24] = (f >> 8) & 0xFF
        img = cell_frame(f)
        i = HDR
        for r in range(ROWS):
            for c in range(COLS):
                v = max(0, min(65535, img[r][c]))
                raw[i] = v & 0xFF
                raw[i + 1] = v >> 8
                i += 2
        fh.write(f"{f / 120.0:.4f} len={FRAME} dev=0 {raw.hex()}\n")
print(path, mode, nframes)
