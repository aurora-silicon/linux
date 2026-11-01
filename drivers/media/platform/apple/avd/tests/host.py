#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the production H.264 instruction builders against a host-only shim.

Run on a Linux build host:
  python3 tests/host.py --baseline BASELINE_AVD_DIR --out OUT
The baseline must contain avd-h264.c and avd-inst.h. No device is opened.
Programs use synthetic addresses and controls, never captured lab data.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

SHIM = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/v4l2-controls.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t dma_addr_t;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define FIELD_PREP(mask, v) (((u32)(v) << __builtin_ctz(mask)) & (mask))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define VISIBLE_IF_KUNIT
#define AVD_ALIGN 128
#define AVD_CODEC_H264 1
#define AVD_QUIRK_LSR BIT(0)
#define AVD_QUIRK_NO_PIPE_STATE BIT(1)
#define AVD_MAX_INST 1024
static unsigned int errors;
#define dev_err_ratelimited(...) (errors++)
#define dev_err(...) (errors++)
struct avd_buf { void *cpu; dma_addr_t addr; size_t size; };
struct vb2_buffer { struct { unsigned int length; } planes[1]; dma_addr_t addr; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct avd_decoded_buffer {
 struct { struct vb2_v4l2_buffer vb; } base;
 struct { u32 start_offset; u32 offsets[4]; } comp;
};
struct avd_run {
 struct { struct vb2_v4l2_buffer *src, *dst; } bufs;
 dma_addr_t comp_out, y_out, uv_out;
};
struct avd_segment { size_t num; u32 instructions[AVD_MAX_INST]; };
struct avd_job { struct avd_segment *segments; unsigned int num; };
struct variant { u32 revision, quirks; };
struct avd_dev { struct variant *variant; void *dev; };
struct format { struct { struct {
 unsigned int width, height;
 struct { unsigned int bytesperline; } plane_fmt[1];
 } pix_mp; } fmt; };
struct m2m { struct { struct { int unused; } q; } cap_q_ctx; };
struct avd_ctx {
 void *priv;
 struct avd_dev *dev;
 struct { struct m2m *m2m_ctx; } fh;
 struct avd_job job;
 struct format decoded_fmt, coded_fmt;
 struct { u32 offsets[4]; } comp;
 bool decomp;
};
static struct avd_decoded_buffer references[16], destination;
static struct avd_decoded_buffer *vb2_to_avd_decoded_buf(struct vb2_buffer *v)
{ (void)v; return &destination; }
static struct avd_decoded_buffer *avd_get_ref_buf(struct avd_ctx *c,
 struct vb2_v4l2_buffer *v, uint64_t ts)
{ (void)c; (void)v; return &references[ts % 16]; }
static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *v, int p)
{ (void)p; return v->addr; }
static struct vb2_buffer *vb2_find_buffer(void *q, uint64_t ts)
{ (void)q; return &references[ts % 16].base.vb.vb2_buf; }
static unsigned int fmt_width(struct avd_ctx *c)
{ return c->decoded_fmt.fmt.pix_mp.width; }
static unsigned int fmt_height(struct avd_ctx *c)
{ return c->decoded_fmt.fmt.pix_mp.height; }
'''

MAIN = r'''
int main(void)
{
 struct variant variant = { .revision = 4 };
 struct avd_dev dev = { .variant = &variant };
 struct avd_h264_ctx hc = {0};
 struct m2m m2m = {0};
 struct avd_segment segments[2];
 struct avd_ctx ctx_storage = { .priv = &hc, .dev = &dev,
  .fh.m2m_ctx = &m2m, .job.segments = segments, .decomp = true };
 struct avd_ctx *ctx = &ctx_storage;
 struct v4l2_ctrl_h264_decode_params dec = {0};
 struct v4l2_ctrl_h264_sps sps = { .chroma_format_idc = 1,
  .pic_width_in_mbs_minus1 = 11, .pic_height_in_map_units_minus1 = 8,
  .flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY };
 struct v4l2_ctrl_h264_pps pps = {0};
 struct v4l2_ctrl_h264_slice_params sl = {0};
 struct v4l2_ctrl_h264_scaling_matrix scaling = {0};
 struct v4l2_ctrl_h264_pred_weights weights = {0};
 struct avd_buf coded = { .addr = 0x123456780ULL, .size = 256 };
 struct avd_h264_run run = { .decode_params = &dec, .sps = &sps,
  .pps = &pps, .slice_params = &sl, .scaling_matrix = &scaling,
  .pred_weights = &weights, .cur_poc = 40, .slice_data_off = 7,
  .base.bufs.dst = &destination.base.vb,
  .base.y_out = 0x20000000, .base.uv_out = 0x20006c00,
  .base.comp_out = 0x20010000, .addresses.mv_color = 0x20020000 };
 hc.active_slice = &coded;
 ctx->decoded_fmt.fmt.pix_mp.width = ctx->coded_fmt.fmt.pix_mp.width = 192;
 ctx->decoded_fmt.fmt.pix_mp.height = ctx->coded_fmt.fmt.pix_mp.height = 144;
 ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 192;
 for (unsigned int i = 0; i < 16; i++) {
  references[i].base.vb.vb2_buf.addr = 0x30000000 + i * 0x100000;
  references[i].base.vb.vb2_buf.planes[0].length = 0x30000;
  references[i].comp.start_offset = 0x10000;
  for (unsigned int j = 0; j < 4; j++)
   references[i].comp.offsets[j] = ctx->comp.offsets[j] = j * 0x1000;
 }
 /* Dense DPBs cover every reference-count, slice type and weight mode.
  * Sparse DPBs deliberately place references on either side of holes. */
 for (unsigned int mask_case = 0; mask_case < 18; mask_case++) {
  unsigned int mask = mask_case <= 16 ? (1U << mask_case) - 1 : 0x8429;
  unsigned int count = __builtin_popcount(mask), ids[16], n = 0;
  for (unsigned int i = 0; i < 16; i++) {
   dec.dpb[i].flags = mask & BIT(i) ?
    V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE : 0;
   dec.dpb[i].reference_ts = i;
   dec.dpb[i].top_field_order_cnt = 2 * i;
   if (mask & BIT(i)) ids[n++] = i;
  }
  for (unsigned int rev = 0; rev < 4; rev++) {
   variant.revision = rev ? 4 : 3;
   variant.quirks = rev < 2 ? AVD_QUIRK_LSR | AVD_QUIRK_NO_PIPE_STATE :
                    rev == 2 ? AVD_QUIRK_LSR : 0;
   for (unsigned int type = 0; type < 3; type++) {
    if (type != V4L2_H264_SLICE_TYPE_I && !count) continue;
    for (unsigned int mode = 0; mode < 4; mode++) {
     for (unsigned int mb = 0; mb < 2; mb++) {
      pps.flags = mode & 1 ? V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE : 0;
      if (mode == 2) pps.flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
      pps.weighted_bipred_idc = mode == 3 ? 2 : 0;
      sl.slice_type = type;
      sl.first_mb_in_slice = mb ? 33 : 0;
      sl.header_bit_size = 53;
      sl.cabac_init_idc = mode % 3;
      sl.slice_qp_delta = mode ? -2 : 3;
      sl.flags = mode & 1 ? V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED : 0;
      sl.num_ref_idx_l0_active_minus1 = count ? count - 1 : 0;
      sl.num_ref_idx_l1_active_minus1 = count > 1 ? 1 : 0;
      for (unsigned int i = 0; i < count; i++) {
       sl.ref_pic_list0[i].index = ids[i];
       sl.ref_pic_list1[i].index = ids[count - 1 - i];
       for (unsigned int j = 0; j < 2; j++) {
        weights.weight_factors[j].luma_weight[i] = i % 2 ? -1 : 1;
        weights.weight_factors[j].luma_offset[i] = i % 3;
        weights.weight_factors[j].chroma_weight[i][0] = 1;
        weights.weight_factors[j].chroma_weight[i][1] = 1;
       }
      }
      run.num_valid = count;
      dec.flags = count ? 0 : V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
      ctx->job.num = 0;
      segments[0].num = segments[1].num = 0;
      stream_hdr(ctx, &run);
      ctx->job.num = 1;
      stream_slice(ctx, &run);
      printf("%u/%u/%u/%u/%u", mask_case, rev, type, mode, mb);
      for (unsigned int seg = 0; seg < 2; seg++)
       for (unsigned int i = 0; i < segments[seg].num; i++)
        printf(" %08x", segments[seg].instructions[i]);
      puts("");
      if (!count) {
       dec.flags = 0;
       ctx->job.num = 0;
       segments[0].num = segments[1].num = 0;
       stream_hdr(ctx, &run);
       ctx->job.num = 1;
       stream_slice(ctx, &run);
       printf("empty-intra/%u/%u/%u", rev, mode, mb);
       for (unsigned int seg = 0; seg < 2; seg++)
        for (unsigned int i = 0; i < segments[seg].num; i++)
         printf(" %08x", segments[seg].instructions[i]);
       puts("");
      }
     }
    }
   }
  }
 }
 for (unsigned int type = 0; type < 10; type++) {
  sl.slice_type = type;
  errors = 0;
  printf("guard/type/%u %d\n", type, VALIDATE_SLICE(ctx, &sl));
 }
 for (unsigned int groups = 0; groups < 8; groups++) {
  pps.num_slice_groups_minus1 = groups;
  printf("guard/groups/%u %d\n", groups, avd_h264_validate_pps(ctx, &pps));
 }
 if (!avd_h264_slice_order_supported(0, 0, 0) ||
     !avd_h264_slice_order_supported(1, 0, 33) ||
     !avd_h264_slice_order_supported(2, 33, 66) ||
     avd_h264_slice_order_supported(0, 0, 33) ||
     avd_h264_slice_order_supported(1, 33, 33) ||
     avd_h264_slice_order_supported(2, 66, 33) ||
     avd_h264_slice_order_supported(2, 66, 0)) return 4;
 /* Every possible DPB occupancy must map to dense header slots. */
 for (unsigned int mask = 0; mask < 65536; mask++) {
  unsigned int count = 0;
  for (unsigned int i = 0; i < 16; i++) {
   dec.dpb[i].flags = mask & BIT(i) ? V4L2_H264_DPB_ENTRY_FLAG_VALID : 0;
   if (mask & BIT(i)) {
    if (avd_h264_ref_slot(&dec, i) != count++) return 2;
   }
  }
  if (avd_h264_num_refs(&dec) != count) return 3;
 }
 return 0;
}
'''


def function(text, name):
    """Extract a named production definition, keeping its body verbatim."""
    import re
    m = re.search(r"^static (?:inline )?\w+[\s\n]+" + name + r"\(", text, re.M)
    if not m:
        raise ValueError(name)
    start = text.index("{", m.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


def build(driver, current, work, label):
    h264 = (driver / "avd-h264.c").read_text()
    inst = (driver / "avd-inst.h").read_text()
    inst = "\n".join(l for l in inst.splitlines() if not l.startswith("#include"))
    body = h264[h264.index("#define H264_SCL_DIMS"):h264.index("static int avd_h264_alloc_bufs")]
    helpers = (current / "avd-h264-params.h").read_text()
    checks = function(h264, "avd_h264_validate_pps")
    if "static int avd_h264_validate_slice(" in h264:
        checks += "\n" + function(h264, "avd_h264_validate_slice")
        checks += "\n#define VALIDATE_SLICE avd_h264_validate_slice\n"
    else:
        checks += "\n#define VALIDATE_SLICE(ctx, sl) 0\n"
    code = "\n".join((SHIM, helpers, inst, body, checks, MAIN))
    src = work / (label + ".c")
    exe = work / label
    src.write_text(code)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-O2", "-Wall",
                    "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
                    str(src), "-o", str(exe)], check=True)
    output = subprocess.check_output([str(exe)], text=True)
    return dict(l.split(" ", 1) for l in output.splitlines())


def hevc_checks(driver, work):
    code = SHIM + function((driver / "avd-hevc.c").read_text(),
                           "avd_hevc_validate_sps") + r"""
int main(void)
{
 struct avd_dev dev = {0};
 struct avd_ctx ctx = { .dev = &dev };
 struct v4l2_ctrl_hevc_sps sps = { .chroma_format_idc = 1,
  .pic_width_in_luma_samples = 192, .pic_height_in_luma_samples = 144 };
 ctx.coded_fmt.fmt.pix_mp.width = 192;
 ctx.coded_fmt.fmt.pix_mp.height = 144;
 for (unsigned int luma = 0; luma < 5; luma++)
  for (unsigned int chroma = 0; chroma < 5; chroma++) {
   sps.bit_depth_luma_minus8 = luma;
   sps.bit_depth_chroma_minus8 = chroma;
   int expected = luma == chroma && (luma == 0 || luma == 2) ? 0 : -EINVAL;
   if (avd_hevc_validate_sps(&ctx, &sps) != expected) return 1;
  }
 return 0;
}
"""
    src = work / "hevc.c"
    exe = work / "hevc"
    src.write_text(code)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-O2",
                    str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    current = Path(__file__).resolve().parent.parent
    a.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        hevc_checks(current, Path(tmp))
        before = build(a.baseline, current, Path(tmp), "before")
        after = build(current, current, Path(tmp), "after")
    unchanged = changed = normalized = 0
    for key, program in after.items():
        if key.startswith("guard/type/"):
            expected = "0" if int(key.rsplit("/", 1)[1]) < 3 else str(-22)
            assert program == expected, (key, program, expected)
        elif key.startswith("guard/groups/"):
            assert program == ("0" if key.endswith("/0") else "-22")
        elif key.startswith("empty-intra/"):
            _, rev, mode, mb = key.split("/")
            # An I picture with no references needs the known IDR program.
            assert program == before[f"0/{rev}/2/{mode}/{mb}"], key
            assert program != before[key], key
            normalized += 1
        elif not key.startswith("17/"):
            assert program == before[key], key
            unchanged += 1
        elif key.split("/")[2] == "2":
            assert program == before[key], key
        else:
            # Only DPB-slot operands of reference-list instructions may change.
            b = before[key].split()
            n = program.split()
            assert len(b) == len(n), key
            for old, new in zip(b, n):
                if old != new:
                    assert int(old, 16) >> 20 == 0x2dc, (key, old, new)
                    assert int(old, 16) & ~15 == int(new, 16) & ~15
            assert b != n, key
            changed += 1
    for label, data in (("before", before), ("after", after)):
        (a.out / (label + ".json")).write_text(json.dumps(data, indent=2) + "\n")
    print(f"PASS: {unchanged} dense-DPB complete programs byte-identical; "
          f"{changed} sparse-DPB programs corrected; "
          f"{normalized} empty-DPB intra programs normalized; all 65536 DPB maps; "
          "10 slice-type, 8 slice-group, 7 slice-order and 25 HEVC bit-depth checks")


if __name__ == "__main__":
    main()
