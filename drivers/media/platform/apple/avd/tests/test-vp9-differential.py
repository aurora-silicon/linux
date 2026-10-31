#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compare complete frame_ops programs with the unchanged stock C builder.

Both builders see identical controls, reference metadata and normalized scratch
DVAs. DMA allocation, V4L2 lookup and transport are simulated. This establishes
program equivalence for valid synthetic tile payloads, not hardware decode CRCs.
"""
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import tempfile
import struct

ROOT = Path(__file__).resolve().parent.parent
common = runpy.run_path(str(ROOT / 'tests/test-vp9-codec.py'), run_name='vp9_helpers')
prefix = common['prefix'] + r'''
#define kzalloc(n,g) calloc(1,n)
#define kfree(p) free(p)
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define writel(a,b) ((void)0)
'''
shim = common['shim'].replace('struct avd_ctx {', r'''
struct avd_segment { unsigned int num; u32 instructions[1024]; };
struct avd_job { enum avd_codec codec; unsigned int num; struct avd_segment *segments; };
struct avd_ctx {''').replace('    struct avd_dev *dev;\n',
    '    struct avd_dev *dev;\n    struct { void *m2m_ctx; } fh;\n    struct avd_job job;\n    int fifo_idx;\n')
shim = shim.replace('static u32 fifo_size(void) { return 0xc00000; }\n', '')
shim += r'''
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(void *p) { return p; }
static void avd_run_preamble(struct avd_ctx *c, struct avd_run *r) { memset(r, 0, sizeof(*r)); }
static void avd_run_postamble(struct avd_ctx *c, struct avd_run *r) { }
static int avd_init_job(struct avd_ctx *c, enum avd_codec codec, size_t segments) { return 0; }
static int avd_submit_job(struct avd_ctx *c) { return 0; }
'''
inst = re.sub(r'^#include .*$', '', (ROOT / 'avd-inst.h').read_text(), flags=re.M)
harness = r'''
int main(void)
{
    u8 data[2048]; u32 words[2048];
    struct avd_variant variant = { .fifo_slots = 15 };
    struct avd_dev dev = { .variant = &variant };
    struct avd_ctx ctx = { .dev = &dev, .image_fmt = AVD_IMG_FMT_420_8BIT };
    struct v4l2_ctrl_vp9_frame frame = { .bit_depth = 8,
        .uncompressed_header_size = 4, .compressed_header_size = 4 };
    struct v4l2_ctrl_vp9_compressed_hdr hdr = {};
    struct vb2_v4l2_buffer src = { .vb2_buf.data = data };
    struct avd_decoded_buffer dst = {};
    struct avd_vp9_run run = { .decode_params = &frame, .prob_updates = &hdr };
#ifdef NATIVE
    struct avd_vp9_slot local = {};
    struct avd_vp9_segmap map = { .buf.addr = 0x1c0000 };
    struct avd_buf program = { .cpu = words, .size = sizeof(words) };
    struct avd_frame f = { .prog = &program };
    run.frame = &f; run.slot = &local;
    local.seg_in = &map; local.seg_out = &map;
#else
    struct avd_vp9_ctx local = {};
    local.bufs.color.addr = 0x1c0000;
#endif
    ctx.priv = &local; ctx.fh.m2m_ctx = &src;
    run.base.bufs.src = &src; run.base.bufs.dst = &dst.base.vb;
    run.base.coded_in = 0x200000; run.base.y_out = 0x400000;
    run.base.uv_out = 0x600000; run.base.comp_out = 0x800000;
    local.bufs.counts.addr = 0x100000; local.bufs.probs.addr = 0x110000;
    local.bufs.above_info.addr = 0x120000; local.bufs.seg.addr = 0x130000;
    local.bufs.pipe_state.addr = 0x140000; local.bufs.ip_above.addr = 0x150000;
    local.bufs.lf_above.addr = 0x160000; local.bufs.lf_left.addr = 0x170000;
    local.bufs.lf_left_info.addr = 0x180000; local.bufs.az_left.addr = 0x190000;
    for (unsigned int gen = 0; gen < 4; gen++)
    for (unsigned int res = 0; res < 2; res++)
    for (unsigned int depth = 0; depth < 2; depth++)
    for (unsigned int intra = 0; intra < 2; intra++)
    for (unsigned int rows = 0; rows <= 2; rows++)
    for (unsigned int cols = 0; cols <= 4; cols++) {
        unsigned int tiles = (1U << rows) * (1U << cols), off = 8;
        variant.revision = gen ? 4 : 3;
        variant.quirks = gen < 2 ? AVD_QUIRK_LSR | AVD_QUIRK_NO_PIPE_STATE :
                         gen == 2 ? AVD_QUIRK_LSR : 0;
        dst.vp9.width = res ? 3840 : 1920; dst.vp9.height = res ? 2160 : 1080;
        ctx.decomp = rows & 1;
        ctx.image_fmt = depth ? AVD_IMG_FMT_420_10BIT : AVD_IMG_FMT_420_8BIT;
        frame.bit_depth = depth ? 10 : 8; frame.profile = depth ? 2 : 0;
        dst.vp9.bit_depth = frame.bit_depth; dst.comp = ctx.comp;
        frame.frame_width_minus_1 = dst.vp9.width - 1;
        frame.frame_height_minus_1 = dst.vp9.height - 1;
        ctx.decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = dst.vp9.width * (depth + 1);
        frame.tile_cols_log2 = cols; frame.tile_rows_log2 = rows;
        frame.flags = V4L2_VP9_FRAME_FLAG_SHOW_FRAME |
            V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING | V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING |
            (intra ? V4L2_VP9_FRAME_FLAG_KEY_FRAME : 0) |
            (cols & 1 ? V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX : 0) |
            (rows & 1 ? V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE : 0);
        frame.seg.flags = cols & 1 ? V4L2_VP9_SEGMENTATION_FLAG_ENABLED |
            V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP : 0;
        frame.seg.feature_enabled[0] = 1U << V4L2_VP9_SEG_LVL_ALT_Q;
        frame.seg.feature_data[0][V4L2_VP9_SEG_LVL_ALT_Q] = -7;
        frame.quant.base_q_idx = 91; frame.quant.delta_q_y_dc = -2;
        frame.lf.level = 19; frame.lf.sharpness = 3; hdr.tx_mode = cols;
        local.cur.flags = frame.flags;
        local.last.valid = true; local.last.flags = V4L2_VP9_FRAME_FLAG_SHOW_FRAME;
        memset(data, 0, sizeof(data));
        for (unsigned int tile = 0; tile < tiles; tile++) {
            if (tile + 1 < tiles) { data[off + 3] = 7; off += 4; }
            off += 7;
        }
        src.vb2_buf.payload = off;
#ifdef NATIVE
        f.prog_words = 0; run.error = 0;
        set_header(&ctx, &run); size_t header = f.prog_words;
        assert(set_tiles(&ctx, &run) == 0);
        assert(vp9_finish_program(&ctx, &f, header, tiles) == 0);
        u32 count = f.prog_words;
#else
        ctx.job.num = 0;
        ctx.job.segments = calloc(tiles + 1, sizeof(*ctx.job.segments));
        assert(ctx.job.segments);
        set_header(&ctx, &run); set_tiles(&ctx, &run);
        u32 mask = variant.revision == 3 ? AVD_OP_EXEC_REV3_VP9_MASK : 0;
        u32 count = 0;
        words[count++] = AVD_OP_EXEC | mask |
            AVD_OP_EXEC_FLAG_START_REV3(variant.revision == 3) |
            AVD_OP_EXEC_FLAG_START_REV4(variant.revision == 4);
        for (unsigned int seg = 0; seg <= ctx.job.num; seg++) {
            struct avd_segment *s = &ctx.job.segments[seg];
            memcpy(words + count, s->instructions, s->num * 4); count += s->num;
            if (seg) words[count++] = AVD_OP_EXEC | mask | AVD_OP_EXEC_FLAG_END(seg == ctx.job.num);
        }
        free(ctx.job.segments); ctx.job.segments = NULL;
#endif
        assert(fwrite(&count, 4, 1, stdout) == 1);
        assert(fwrite(words, 4, count, stdout) == count);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avd-vp9-diff-') as tmp:
    results = []
    for native in [False, True]:
        codec = (ROOT / ('avd-vp9.c' if native else 'avd-vp9-legacy.c')).read_text()
        codec = re.sub(r'^#include .*$', '', codec, flags=re.M)
        test_shim = shim if native else shim.replace(
            common['shim'][common['shim'].index('static const struct avd_coded_fmt_ops avd_vp9_legacy_fmt_ops = {'):common['shim'].index('};', common['shim'].index('static const struct avd_coded_fmt_ops avd_vp9_legacy_fmt_ops = {')) + 2], '')
        c = Path(tmp) / f'{native}.c'
        c.write_text(prefix + common['uapi_defines'] + '\n' + common['uapi_types'] + '\n' +
                     common['vp9_header'] + test_shim + inst + common['helpers'] +
                     codec + ('\n#define NATIVE\n' if native else '\n') + harness)
        exe = Path(tmp) / str(native)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-function',
                        '-Wno-unused-variable', '-Wno-missing-field-initializers',
                        *shlex.split(os.environ.get('CFLAGS', '')), str(c), '-o', str(exe)], check=True)
        results.append(subprocess.check_output([str(exe)]))
    if results[0] != results[1]:
        offsets = [0, 0]
        for case in range(480):
            counts = [struct.unpack_from('<I', result, offsets[i])[0] for i, result in enumerate(results)]
            programs = [struct.unpack_from('<' + 'I' * counts[i], result, offsets[i] + 4) for i, result in enumerate(results)]
            if programs[0] != programs[1]:
                differences = [(i, hex(a), hex(b)) for i, (a, b) in enumerate(zip(*programs)) if a != b]
                raise AssertionError(f'case {case}, lengths {counts}, stock/native differences {differences[:8]}')
            offsets = [offsets[i] + 4 * (counts[i] + 1) for i in range(2)]
print('VP9 differential: 480 complete 1080p/4K programs match stock at FIFO 0 across V3, M1 Pro, M2 and V4, 8/10-bit, key/inter, tiles and segmentation PASS')
