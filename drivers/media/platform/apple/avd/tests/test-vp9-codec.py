#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the complete production codec with real V4L2 probability helpers.

Only allocation, DMA addresses, buffer metadata and control lookup are emulated.
The kernel's own UAPI structures/defaults/forward and backward adaptation run on
host C. No target, libvpx, privileged operation or hardware access is used.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REPO = Path(os.environ.get('AVD_TEST_KERNEL_SRC', ROOT.parents[4]))

def kernel_file(path):
    file = REPO / path
    if file.exists():
        return file.read_text()
    return subprocess.check_output(['git', '-C', str(REPO), 'show', 'HEAD:' + path], text=True)

def structure(source, name):
    start = source.index('struct ' + name + ' {')
    end = source.index('\n};', start) + 3
    return source[start:end]

uapi = kernel_file('include/uapi/linux/v4l2-controls.h')
vp9_header = kernel_file('include/media/v4l2-vp9.h')
helpers = kernel_file('drivers/media/v4l2-core/v4l2-vp9.c')
codec = (ROOT / 'avd-vp9.c').read_text()
inst = (ROOT / 'avd-inst.h').read_text().split('static inline u32 fifo_size', 1)[0]
inst = re.sub(r'^#include .*$', '', inst, flags=re.M) + '\n#endif\n'
codec = re.sub(r'^#include .*$', '', codec, flags=re.M)
helpers = re.sub(r'^#include .*$', '', helpers, flags=re.M)
vp9_header = re.sub(r'^#include .*$', '', vp9_header, flags=re.M)
uapi_types = '\n'.join(structure(uapi, name) for name in [
    'v4l2_vp9_loop_filter', 'v4l2_vp9_quantization', 'v4l2_vp9_segmentation',
    'v4l2_ctrl_vp9_frame', 'v4l2_vp9_mv_probs', 'v4l2_ctrl_vp9_compressed_hdr'])
uapi_defines = '\n'.join(line for line in uapi.splitlines()
                         if line.startswith('#define V4L2_VP9_'))
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8, __u8;
typedef int8_t __s8;
typedef uint16_t u16, __u16;
typedef int16_t __s16;
typedef uint32_t u32, __u32, __le32;
typedef int32_t s32;
typedef uint64_t u64, __u64, dma_addr_t;
#define U32_MAX UINT32_MAX
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31 - (h))))
#define FIELD_PREP(m,v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define clamp(v,lo,hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define DIV_ROUND_UP(a,b) (((a) + (b) - 1) / (b))
#define round_up(a,b) (DIV_ROUND_UP(a,b) * (b))
#define WARN_ON(v) (!!(v))
#undef static_assert
#define static_assert(v) _Static_assert(v, #v)
#define noinline_for_stack
#define EXPORT_SYMBOL_GPL(v)
#define MODULE_DESCRIPTION(v)
#define MODULE_AUTHOR(v)
#define MODULE_LICENSE(v)
#define cpu_to_le32(v) (v)
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((v) >> 32))
#define GFP_KERNEL 0
#define kvzalloc(n,g) calloc(1,n)
#define kvfree(p) free(p)
#define dev_err(...) ((void)0)
#define AVD_MAX_FRAMES 16
#define AVD_VP9_PROBS_SIZE 0x774
#define AVD_QUIRK_LSR 1
#define AVD_QUIRK_NO_PIPE_STATE 2
#define V4L2_CID_STATELESS_VP9_FRAME 1
#define V4L2_CID_STATELESS_VP9_COMPRESSED_HDR 2
'''
shim = r'''
struct avd_ctx;
struct avd_codec_frame_ops;
struct avd_frame;
enum avd_image_fmt { AVD_IMG_FMT_ANY, AVD_IMG_FMT_420_8BIT, AVD_IMG_FMT_420_10BIT,
                    AVD_IMG_FMT_422_8BIT, AVD_IMG_FMT_422_10BIT };
enum avd_codec { AVD_CODEC_HEVC, AVD_CODEC_H264, AVD_CODEC_VP9, AVD_CODEC_AV1 };
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
enum avd_frame_dep { AVD_DEP_NONE, AVD_DEP_PREV_DONE };
struct vb2_buffer { u8 *data; size_t payload; u64 timestamp; dma_addr_t dma; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct avd_buf { void *cpu; dma_addr_t addr; size_t size; };
struct avd_frame {
    unsigned int idx;
    enum avd_frame_dep dep;
    struct avd_buf *prog, *fp;
    dma_addr_t fifo_iova;
    u32 cmd[24];
    size_t prog_words;
    void *codec_priv;
    struct vb2_v4l2_buffer *src, *dst;
};
struct avd_comp { size_t start_offset; u32 offsets[4]; };
struct avd_decoded_buffer {
    struct { struct vb2_v4l2_buffer vb; } base;
    struct avd_comp comp;
    struct { unsigned int width, height, bit_depth; } vp9;
};
struct avd_run {
    struct { struct vb2_v4l2_buffer *src, *dst; } bufs;
    dma_addr_t coded_in, y_out, uv_out, comp_out;
};
struct avd_variant { unsigned int revision, quirks, fifo_slots; };
struct avd_dev { const struct avd_variant *variant; };
struct v4l2_ctrl {
    unsigned int id;
    struct { void *p; } p_cur;
    struct { struct v4l2_ctrl_vp9_frame *p_vp9_frame; } p_new;
};
struct v4l2_ctrl_handler { struct v4l2_ctrl ctrl[2]; };
struct avd_ctx {
    struct avd_dev *dev;
    const struct avd_codec_frame_ops *frame_ops;
    struct v4l2_ctrl_handler ctrl_hdl;
    void *priv;
    enum avd_image_fmt image_fmt;
    bool decomp;
    struct avd_comp comp;
    struct { struct { struct { unsigned int width, height;
        struct { unsigned int bytesperline; } plane_fmt[1]; } pix_mp; } fmt; } decoded_fmt;
};
struct avd_codec_frame_ops {
    int (*prepare)(struct avd_ctx *, struct avd_frame *);
    int (*pack_cmd)(struct avd_ctx *, struct avd_frame *);
    void (*retire)(struct avd_ctx *, struct avd_frame *, enum vb2_buffer_state);
};
struct avd_coded_fmt_ops {
    int (*start)(struct avd_ctx *);
    void (*stop)(struct avd_ctx *);
    void (*done)(struct avd_ctx *, struct vb2_v4l2_buffer *, struct vb2_v4l2_buffer *, enum vb2_buffer_state);
    void (*submit)(struct avd_ctx *);
    unsigned int (*pp_kicks)(struct avd_ctx *);
    int (*run)(struct avd_ctx *);
    enum avd_image_fmt (*get_image_fmt)(struct avd_ctx *, struct v4l2_ctrl *);
};
static struct v4l2_ctrl *v4l2_ctrl_find(struct v4l2_ctrl_handler *h, unsigned int id)
{ return &h->ctrl[id - 1]; }
static struct avd_decoded_buffer *vb2_to_avd_decoded_buf(struct vb2_buffer *v)
{ return (struct avd_decoded_buffer *)v; }
static struct avd_decoded_buffer *avd_get_ref_buf(struct avd_ctx *c, struct vb2_v4l2_buffer *v, u64 ts)
{ return (struct avd_decoded_buffer *)v; }
static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *v, int p) { return v->dma; }
static void *vb2_plane_vaddr(struct vb2_buffer *v, int p) { return v->data; }
static size_t vb2_get_plane_payload(struct vb2_buffer *v, int p) { return v->payload; }
static u32 get_unaligned_be32(const u8 *v)
{ return (u32)v[0] << 24 | (u32)v[1] << 16 | (u32)v[2] << 8 | v[3]; }
static u32 fmt_width(struct avd_ctx *c) { return c->decoded_fmt.fmt.pix_mp.width; }
static u32 fmt_height(struct avd_ctx *c) { return c->decoded_fmt.fmt.pix_mp.height; }
static int avd_buf_alloc(struct avd_dev *d, struct avd_buf *b, size_t n)
{
    static dma_addr_t next = 0x1000000;
    b->cpu = calloc(1, n); b->size = n; b->addr = next;
    next += round_up(n, 4096);
    return b->cpu ? 0 : -ENOMEM;
}
static void avd_buf_free(struct avd_dev *d, struct avd_buf *b)
{ free(b->cpu); memset(b, 0, sizeof(*b)); }
extern const struct avd_codec_frame_ops avd_vp9_frame_ops;
static unsigned int legacy_calls;
static int legacy_start(struct avd_ctx *c) { legacy_calls |= 1; return 0; }
static void legacy_stop(struct avd_ctx *c) { legacy_calls |= 2; }
static int legacy_run(struct avd_ctx *c) { legacy_calls |= 4; return 0; }
static void legacy_done(struct avd_ctx *c, struct vb2_v4l2_buffer *s,
    struct vb2_v4l2_buffer *d, enum vb2_buffer_state state) { legacy_calls |= 8; }
static void legacy_submit(struct avd_ctx *c) { legacy_calls |= 16; }
static unsigned int legacy_kicks(struct avd_ctx *c) { legacy_calls |= 32; return 2; }
static const struct avd_coded_fmt_ops avd_vp9_legacy_fmt_ops = {
    .start = legacy_start, .stop = legacy_stop, .run = legacy_run,
    .done = legacy_done, .submit = legacy_submit, .pp_kicks = legacy_kicks,
};
static bool avd_backend_is_qfw(const struct avd_dev *d) { return true; }
static u32 fifo_size(void) { return 0xc00000; }
static bool avd_backend_is_apple(const struct avd_dev *d) { return false; }
static int avd_vp9_pack_cmd(struct avd_ctx *c, struct avd_frame *f) { return -EOPNOTSUPP; }
static int avd_vp9_pack_fp(struct avd_ctx *c, struct avd_frame *f,
    const struct v4l2_ctrl_vp9_frame *v, const struct v4l2_ctrl_vp9_compressed_hdr *h,
    const void *p, dma_addr_t a) { return -EOPNOTSUPP; }
static bool avd_vp9_needs_counts(u32 flags)
{ return (flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX) && !(flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE); }
'''
tests = r'''
int main(void)
{
    struct avd_variant variant = { .revision = 3, .quirks = 3, .fifo_slots = 7 };
    struct avd_dev dev = { .variant = &variant };
    struct avd_ctx ctx = { .dev = &dev, .frame_ops = &avd_vp9_frame_ops, .image_fmt = AVD_IMG_FMT_420_8BIT };
    struct v4l2_ctrl_vp9_frame control = { .bit_depth = 8,
        .frame_width_minus_1 = 63, .frame_height_minus_1 = 63,
        .compressed_header_size = 4, .uncompressed_header_size = 4 };
    struct v4l2_ctrl_vp9_compressed_hdr hdr = {};
    struct avd_frame frames[16] = {};
    struct avd_buf programs[16] = {};
    struct vb2_v4l2_buffer sources[16] = {};
    struct avd_decoded_buffer outputs[16] = {};
    struct v4l2_vp9_frame_context contexts[4];
    u8 payload[32] = {};
    ctx.decoded_fmt.fmt.pix_mp.width = 64;
    ctx.decoded_fmt.fmt.pix_mp.height = 64;
    ctx.decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 64;
    ctx.ctrl_hdl.ctrl[0].p_cur.p = &control;
    ctx.ctrl_hdl.ctrl[1].p_cur.p = &hdr;
    assert(avd_vp9_start(&ctx) == 0);
    for (unsigned int c = 0; c < 4; c++) contexts[c] = v4l2_vp9_default_probs;
    for (unsigned int i = 0; i < 16; i++) {
        programs[i].cpu = calloc(1, 4096); programs[i].size = 4096;
        sources[i].vb2_buf.data = payload; sources[i].vb2_buf.payload = 32;
        sources[i].vb2_buf.dma = 0x200000 + i * 4096;
        outputs[i].base.vb.vb2_buf.dma = 0x400000 + i * 4096;
        frames[i].idx = i; frames[i].src = &sources[i];
        frames[i].dst = &outputs[i].base.vb; frames[i].prog = &programs[i];
        control.flags = V4L2_VP9_FRAME_FLAG_SHOW_FRAME |
            V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING | V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING |
            V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX | V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
        if (!i) control.flags |= V4L2_VP9_FRAME_FLAG_KEY_FRAME;
        control.seg.flags = i % 2 ? V4L2_VP9_SEGMENTATION_FLAG_ENABLED |
            V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP : 0;
        control.seg.tree_probs[0] = i + 30;
        control.seg.pred_probs[0] = i + 50;
        control.seg.feature_enabled[0] = 1U << V4L2_VP9_SEG_LVL_ALT_Q;
        control.seg.feature_data[0][V4L2_VP9_SEG_LVL_ALT_Q] = i;
        control.frame_context_idx = i % 4;
        hdr.skip[0] = i + 1;
        unsigned int idx = v4l2_vp9_reset_frame_ctx(&control, contexts);
        v4l2_vp9_fw_update_probs(&contexts[idx], &hdr, &control);
        assert(avd_vp9_prepare(&ctx, &frames[i]) == 0);
        assert(frames[i].dep == AVD_DEP_NONE);
        assert(avd_vp9_frame_pp_kicks(&frames[i]) == 1);
        assert(frames[i].fifo_iova);
        assert(frames[i].cmd[0] == AVD_CODEC_VP9);
        struct avd_vp9_slot *slot = frames[i].codec_priv;
        struct avd_vp9_probs *packed = slot->bufs.probs.cpu;
        assert(packed->seg.tree_probs[0] == i + 30);
        assert(packed->seg.pred_probs[0] == i + 50);
        assert(memcmp(&slot->probability_tables, &contexts[idx], sizeof(contexts[idx])) == 0);
        for (unsigned int j = 0; j < i; j++) {
            assert(frames[j].fifo_iova != frames[i].fifo_iova);
            struct avd_vp9_slot *prior = frames[j].codec_priv;
            assert(prior->bufs.probs.cpu != slot->bufs.probs.cpu);
            assert(prior->bufs.counts.cpu != slot->bufs.counts.cpu);
        }
    }
    /* Later preparation did not overwrite the first frame's entropy snapshot. */
    struct avd_vp9_slot *first = frames[0].codec_priv;
    assert(first->probability_tables.skip[0] != contexts[0].skip[0]);
    for (unsigned int i = 0; i < 16; i++) avd_vp9_retire(&ctx, &frames[i], VB2_BUF_STATE_DONE);
    /* A truncated/show-existing-only payload must not mutate stream state. */
    struct avd_vp9_ctx *stream = ctx.priv;
    struct v4l2_vp9_frame_context saved[4];
    memcpy(saved, stream->frame_context, sizeof(saved));
    sources[2].vb2_buf.payload = 1;
    assert(avd_vp9_prepare(&ctx, &frames[2]) == -EINVAL);
    assert(memcmp(saved, stream->frame_context, sizeof(saved)) == 0);
    unsigned int pins = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(stream->segmaps); i++) pins += stream->segmaps[i].users;
    assert(pins == 1);
    sources[2].vb2_buf.payload = 32;
    /* A backward refresh must finish before CPU preparation consumes its counts. */
    control.flags &= ~(V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE);
    control.frame_context_idx = 0; hdr.skip[0] = 9;
    assert(avd_vp9_prepare(&ctx, &frames[0]) == 0);
    assert(frames[0].dep == AVD_DEP_PREV_DONE);
    struct avd_vp9_slot *producer = frames[0].codec_priv;
    struct v4l2_vp9_frame_context expected = producer->initial_probs;
    struct avd_vp9_frame_symbol_counts *native = producer->bufs.counts.cpu;
    native->ref_cnt[0] = 100; native->ref_cnt[1] = 5;
    native->ref_cnt[2] = 2; native->ref_cnt[3] = 107; native->eob_0[0] = 109;
    v4l2_vp9_adapt_coef_probs(&expected, &producer->cnts, false, false);
    assert(avd_vp9_prepare(&ctx, &frames[1]) == -EAGAIN);
    assert(frames[1].dep == AVD_DEP_PREV_DONE);
    avd_vp9_retire(&ctx, &frames[0], VB2_BUF_STATE_DONE);
    assert(stream->frame_context[0].coef[0][0][0][0][0][0] == expected.coef[0][0][0][0][0][0]);
    control.flags &= ~V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
    assert(avd_vp9_prepare(&ctx, &frames[1]) == 0);
    assert(frames[1].dep == AVD_DEP_NONE);
    avd_vp9_retire(&ctx, &frames[1], VB2_BUF_STATE_ERROR);
    assert(avd_vp9_prepare(&ctx, &frames[2]) == -EIO);
    avd_vp9_stop(&ctx);
    /* Profile 2 uses 10-bit scratch sizing and the existing bit-depth fields. */
    ctx.image_fmt = AVD_IMG_FMT_420_10BIT;
    ctx.decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 128;
    control.profile = 2; control.bit_depth = 10;
    control.flags = V4L2_VP9_FRAME_FLAG_KEY_FRAME |
        V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING | V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
    assert(avd_vp9_start(&ctx) == 0);
    assert(avd_vp9_prepare(&ctx, &frames[0]) == 0);
    u32 common = ((u32 *)programs[0].cpu)[6];
    assert((common & GENMASK(23, 19)) == (2U << 19));
    assert((common & GENMASK(18, 15)) == (2U << 15));
    avd_vp9_retire(&ctx, &frames[0], VB2_BUF_STATE_DONE);
    control.bit_depth = 12;
    assert(avd_vp9_prepare(&ctx, &frames[1]) == -EINVAL);
    avd_vp9_stop(&ctx);
    /* Stock transport dispatch must reach every original callback. */
    ctx.frame_ops = NULL;
    assert(avd_vp9_fmt_ops.start(&ctx) == 0);
    assert(avd_vp9_fmt_ops.run(&ctx) == 0);
    avd_vp9_fmt_ops.done(&ctx, &sources[0], &outputs[0].base.vb, VB2_BUF_STATE_DONE);
    avd_vp9_fmt_ops.submit(&ctx);
    assert(avd_vp9_fmt_ops.pp_kicks(&ctx) == 2);
    avd_vp9_fmt_ops.stop(&ctx);
    assert(legacy_calls == 63);
    for (unsigned int i = 0; i < 16; i++) free(programs[i].cpu);
    puts("VP9 codec: real V4L2 entropy helpers, 16 prepared frames, segmentation, count-producer retry, error retirement, Profile 2 PASS");
    return 0;
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='avd-vp9-codec-') as tmp:
        c = Path(tmp) / 'codec.c'
        c.write_text(prefix + uapi_defines + '\n' + uapi_types + '\n' + vp9_header + '\n' +
                     shim + '\n' + inst + '\n' + helpers + '\n' + codec + '\n' + tests)
        exe = Path(tmp) / 'codec'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-function',
                        '-Wno-missing-field-initializers', *shlex.split(os.environ.get('CFLAGS', '')), str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
