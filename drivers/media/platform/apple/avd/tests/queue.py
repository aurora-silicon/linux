#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise queue initialization in FFmpeg's CREATE_BUFS-before-S_FMT order."""
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

SHIM = r'''
#include <stdbool.h>
#include <stdio.h>
#include <linux/videodev2.h>
#define VB2_MMAP 1
#define VB2_DMABUF 2
#define VB2_MAX_FRAME 32
#define VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF 1
#define max(a, b) ((a) > (b) ? (a) : (b))
struct vb2_queue {
 unsigned int type, io_modes, dma_attrs, buf_struct_size, timestamp_flags;
 unsigned int subsystem_flags, max_num_buffers;
 void *drv_priv, *lock, *dev;
 const void *ops, *mem_ops;
 bool supports_requests, allow_cache_hints, bidirectional;
};
struct avd_dev { int vdev_lock; void *dev; struct { void *dev; } v4l2_dev; };
struct avd_coded_fmt_desc { unsigned int subsystem_flags; };
struct avd_ctx { struct avd_dev *dev; const struct avd_coded_fmt_desc *coded_fmt_desc; };
struct v4l2_m2m_buffer { int unused; };
struct avd_decoded_buffer { int unused; };
static unsigned int max_capture_buffers = 128;
static const int avd_queue_ops, vb2_dma_contig_memops;
static unsigned int flags_at_init;
static int vb2_queue_init(struct vb2_queue *q)
{
 if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
  flags_at_init = q->subsystem_flags;
 return 0;
}
'''
MAIN = r'''
int main(void)
{
 const struct avd_coded_fmt_desc h264 = { 1 }, vp9 = { 0 };
 struct avd_dev dev = {0};
 struct avd_ctx ctx = { .dev = &dev, .coded_fmt_desc = &h264 };
 struct vb2_queue src = {0}, dst = {0};
 if (avd_queue_init(&ctx, &src, &dst)) return 1;
 /* CREATE_BUFS count=0 returns these capabilities. FFmpeg caches them. */
 unsigned int cached_hold = src.subsystem_flags & 1;
 select_features(&src, &h264);
 printf("%u %u", flags_at_init, cached_hold);
 select_features(&src, &vp9);
 printf(" %u", src.subsystem_flags);
 select_features(&src, &h264);
 printf(" %u", src.subsystem_flags);
 /* FFmpeg uses first=true,last=true for every slice without cached HOLD. */
 const unsigned int pictures[] = { 1, 3, 4, 9, 10, 12, 20 };
 for (unsigned int p = 0; p < sizeof(pictures)/sizeof(pictures[0]); p++) {
  unsigned int submissions = 0;
  for (unsigned int s = 0; s < pictures[p]; s++) {
   bool last = !cached_hold || s + 1 == pictures[p];
   if (last) submissions++;
  }
  printf(" %u", submissions);
 }
 puts("");
 return 0;
}
'''


def run(driver, work, label, function):
    source = (driver / "avd-v4l2.c").read_text()
    assignment = re.search(r"vq->subsystem_flags\s*\|?=\s*desc->subsystem_flags;", source)[0]
    code = SHIM + function((driver / "avd-drv.c").read_text(), "avd_queue_init")
    code += "\nstatic void select_features(struct vb2_queue *vq, const struct avd_coded_fmt_desc *desc) {\n"
    code += assignment + "\n}\n" + MAIN
    src, exe = work / (label + ".c"), work / label
    src.write_text(code)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", str(src), "-o", str(exe)], check=True)
    return [int(x) for x in subprocess.check_output([str(exe)], text=True).split()]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    current = Path(__file__).resolve().parent.parent
    function = runpy.run_path(str(current / "tests/host.py"))["function"]
    with tempfile.TemporaryDirectory() as tmp:
        before = run(a.baseline, Path(tmp), "before", function)
        after = run(current, Path(tmp), "after", function)
    assert before == [0, 0, 1, 1, 1, 3, 4, 9, 10, 12, 20], before
    assert after == [1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1], after
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "queue.json").write_text(json.dumps({"before": before, "after": after}, indent=2) + "\n")
    print("PASS: CREATE_BUFS before S_FMT exposes HOLD; multislice pictures submit once; format switch clears stale flags")


if __name__ == "__main__":
    main()
