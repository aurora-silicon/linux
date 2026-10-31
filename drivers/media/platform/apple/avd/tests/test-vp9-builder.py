#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the production tile writer and compare it with a legacy-word oracle.

No kernel or target is needed. This deliberately exercises real C, including
truncated tile headers, overflows, output bounds and the 4 GiB address carry.
"""
import os
import shlex
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / 'avd-vp9.c').read_text()

def function(name):
    start = source.index('static ', source.index(name) - 100)
    # Find the last static declaration before the exact definition.
    end_name = source.index(name + '(')
    start = source.rfind('static ', 0, end_name)
    brace = source.index('{', end_name)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
typedef uint64_t dma_addr_t;
#define U32_MAX UINT32_MAX
#define cpu_to_le32(v) (v)
#define AVD_QUIRK_LSR 1
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define AVD_MAX_FRAMES 16
#define AVD_OP_EXEC 0x2b000000U
#define AVD_OP_EXEC_REV3_VP9_MASK 0xfff000U
#define AVD_OP_EXEC_FIFO_IDX(v) ((v) << 4)
#define AVD_OP_EXEC_FLAG_END(v) ((v) ? 0x400 : 0)
#define AVD_OP_EXEC_FLAG_START_REV3(v) ((v) ? 0x100 : 0)
#define AVD_OP_EXEC_FLAG_START_REV4(v) ((v) ? 0x200 : 0)
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((v) >> 32))
#define AVD_OP_CODED_DATA 0x2d800000U
#define AVD_OP_CODED_DATA_ADDR(v) ((v) & 0x1fff)
#define AVD_OP_SL_DIM_START 0x2a000000U
#define AVD_OP_SL_DIM_START_Y(v) (((v) & 0xfff) << 12)
#define AVD_OP_SL_DIM_START_X(v) ((v) & 0xfff)
#define AVD_SL_DIM_END_Y(v) (((v) & 0xfff) << 12)
#define AVD_SL_DIM_END_X(v) ((v) & 0xfff)
#define AVD_SL_DIM_END_COL(v) (((v) & 15) << 24)
struct avd_buf { void *cpu; size_t size; };
struct avd_frame { unsigned int idx; struct avd_buf *prog; size_t prog_words; };
struct vb2_buffer { u8 *data; size_t payload; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct avd_variant { unsigned int quirks, revision, fifo_slots; };
struct avd_dev { const struct avd_variant *variant; };
struct avd_ctx { struct avd_dev *dev; };
static bool avd_backend_is_qfw(const struct avd_dev *d) { return true; }
struct avd_vp9_segmap { unsigned int users; };
struct avd_vp9_slot { struct avd_vp9_segmap *seg_in, *seg_out; };
struct avd_vp9_ctx {
    struct avd_vp9_segmap segmaps[AVD_MAX_FRAMES + 1], *last_segmap;
};
struct v4l2_ctrl_vp9_frame {
    u32 uncompressed_header_size, compressed_header_size;
    u32 frame_width_minus_1, frame_height_minus_1;
    u8 tile_rows_log2, tile_cols_log2;
};
struct avd_vp9_run {
    struct avd_frame *frame;
    int error;
    const struct v4l2_ctrl_vp9_frame *decode_params;
    struct { struct { struct vb2_v4l2_buffer *src; } bufs;
             dma_addr_t coded_in; } base;
};
static const u8 *vb2_plane_vaddr(struct vb2_buffer *v, int plane)
{ (void)plane; return v->data; }
static size_t vb2_get_plane_payload(struct vb2_buffer *v, int plane)
{ (void)plane; return v->payload; }
static u32 get_unaligned_be32(const u8 *v)
{ return (u32)v[0] << 24 | (u32)v[1] << 16 | (u32)v[2] << 8 | v[3]; }
#define push(word, name) vp9_push(run, word)
'''
tests = r'''
static void test_maps(void)
{
    struct avd_vp9_ctx stream = {};
    struct avd_vp9_slot slots[AVD_MAX_FRAMES] = {};
    struct avd_vp9_segmap *old;
    for (unsigned int i = 0; i < AVD_MAX_FRAMES; i++) {
        assert(vp9_get_maps(&stream, &slots[i]) == 0);
        if (i) assert(slots[i].seg_in == slots[i - 1].seg_out);
        vp9_publish_map(&stream, &slots[i]);
    }
    old = slots[0].seg_out;
    vp9_put_maps(&slots[0]);
    assert(old->users == 1); /* still input to frame 1 */
    assert(vp9_get_maps(&stream, &slots[0]) == 0);
    assert(slots[0].seg_out != old);
    vp9_publish_map(&stream, &slots[0]);
    for (unsigned int i = 1; i < AVD_MAX_FRAMES; i++) vp9_put_maps(&slots[i]);
    vp9_put_maps(&slots[0]);
    assert(stream.last_segmap->users == 1);
    stream.last_segmap->users--;
    stream.last_segmap = NULL;
    for (unsigned int i = 0; i <= AVD_MAX_FRAMES; i++)
        assert(stream.segmaps[i].users == 0);
    /* A failed preparation releases its pins without publishing an output. */
    assert(vp9_get_maps(&stream, &slots[0]) == 0);
    vp9_put_maps(&slots[0]);
    assert(stream.last_segmap == NULL);
}
int main(void)
{
    test_maps();
    u8 data[4096];
    u32 words[2048], expected[2048];
    struct avd_buf prog = { .cpu = words, .size = sizeof(words) };
    struct avd_frame f = { .prog = &prog };
    struct avd_variant variant = {};
    struct avd_dev dev = { .variant = &variant };
    struct avd_ctx ctx = { .dev = &dev };
    struct vb2_v4l2_buffer src = { .vb2_buf.data = data };
    struct v4l2_ctrl_vp9_frame ctrl = {
        .uncompressed_header_size = 3, .compressed_header_size = 5,
        .frame_width_minus_1 = 4095, .frame_height_minus_1 = 1023,
    };
    struct avd_vp9_run run = { .frame = &f, .decode_params = &ctrl,
        .base = { .bufs.src = &src, .coded_in = 0x12340000 } };
    for (unsigned int rows = 0; rows <= 2; rows++) {
        for (unsigned int cols = 0; cols <= 4; cols++) {
            unsigned int nrows = 1U << rows, ncols = 1U << cols;
            unsigned int tiles = nrows * ncols, off = 8, pos = 0;
            ctrl.tile_rows_log2 = rows;
            ctrl.tile_cols_log2 = cols;
            memset(data, 0, sizeof(data));
            for (unsigned int tile = 0; tile < tiles; tile++) {
                if (tile + 1 < tiles) {
                    data[off + 3] = 7; off += 4;
                }
                unsigned int row = tile / ncols, col = tile % ncols;
                expected[pos++] = 0x2d800000;
                expected[pos++] = 0x12340000 + off;
                expected[pos++] = 7;
                expected[pos++] = 0x2a000000 |
                    ((row * 16 / nrows) << 12) | (col * 64 / ncols);
                expected[pos++] = col << 24 |
                    (((row + 1) * 16 / nrows - 1) << 12) |
                    ((col + 1) * 64 / ncols - 1);
                off += 7;
            }
            src.vb2_buf.payload = off;
            f.prog_words = 0; run.error = 0;
            assert(set_tiles(&ctx, &run) == 0);
            assert(f.prog_words == pos);
            assert(memcmp(words, expected, pos * 4) == 0);
            /* Flatten the same legacy EXEC boundaries, including FIFO mask. */
            memmove(words + 49, words, pos * 4);
            for (unsigned int h = 0; h < 49; h++) words[h] = 0x12340000 + h;
            f.prog_words = 49 + pos;
            variant.revision = 3; variant.fifo_slots = 7; f.idx = 6;
            assert(vp9_finish_program(&ctx, &f, 49, tiles) == 0);
            assert(f.prog_words == 50 + tiles * 6);
            assert(words[0] == 0x2bfff160);
            for (unsigned int h = 0; h < 49; h++)
                assert(words[h + 1] == 0x12340000 + h);
            for (unsigned int t = 0; t < tiles; t++) {
                assert(memcmp(words + 50 + t * 6, expected + t * 5, 20) == 0);
                assert(words[55 + t * 6] ==
                    (0x2bfff060U | (t + 1 == tiles ? 0x400U : 0)));
            }
            prog.size = 4;
            f.prog_words = 0; run.error = 0;
            assert(set_tiles(&ctx, &run) == -ENOSPC);
            assert(f.prog_words == 1);
            prog.size = sizeof(words);
        }
    }
    ctrl.tile_cols_log2 = 1; ctrl.tile_rows_log2 = 0;
    src.vb2_buf.payload = 10; /* only two bytes of a tile length */
    f.prog_words = 0; run.error = 0;
    assert(set_tiles(&ctx, &run) == -EINVAL);
    src.vb2_buf.payload = 20;
    memset(data + 8, 0xff, 4);
    assert(set_tiles(&ctx, &run) == -EINVAL);
    memset(data + 8, 0, 4); /* empty tile */
    assert(set_tiles(&ctx, &run) == -EINVAL);
    src.vb2_buf.payload = 1; /* show-existing is handled in userspace */
    assert(set_tiles(&ctx, &run) == -EINVAL);
    ctrl.tile_cols_log2 = 0;
    src.vb2_buf.payload = 15;
    run.base.coded_in = 0xfffffff8ULL;
    f.prog_words = 0; run.error = 0;
    assert(set_tiles(&ctx, &run) == 0);
    assert(words[0] == 0x2d800001 && words[1] == 0);
    puts("VP9 builder: 15 tile geometries match legacy words; malformed input, capacity, address carry, EXEC framing and segmentation lifetime PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avd-vp9-host-') as tmp:
    c = Path(tmp) / 'test.c'
    c.write_text(prefix + function('vp9_push') + '\n' + function('set_tiles') + '\n' + function('vp9_finish_program') + '\n' + function('vp9_get_maps') + '\n' + function('vp9_put_maps') + '\n' + function('vp9_publish_map') + tests)
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', *shlex.split(os.environ.get('CFLAGS', '')), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
