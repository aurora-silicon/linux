/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef HEVC_SHIM_H_
#define HEVC_SHIM_H_
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <linux/v4l2-controls.h>
#include "avd-hevc-fp.h"
#define AVD_H_
typedef int8_t s8;
typedef int32_t s32;
typedef uint64_t dma_addr_t;
typedef uint32_t __le32;
#define cpu_to_le32(v) (v)
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((u64)(v) >> 32))
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define FIELD_PREP(mask, v) (((u32)(v) << __builtin_ctz(mask)) & (mask))
#define DIV_ROUND_UP(v, d) (((v) + (d) - 1) / (d))
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define round_down(v, a) ((v) & ~((a) - 1))
#define AVD_ALIGN 256
#define AVD_MAX_INST 512
#define AVD_QUIRK_LSR BIT(0)
#define AVD_QUIRK_NO_PIPE_STATE BIT(1)
#define AVD_QUIRK_HEVC_V5_HEADER BIT(2)
#define dev_err(...) ((void)0)
enum avd_codec { AVD_CODEC_HEVC, AVD_CODEC_H264, AVD_CODEC_VP9, AVD_CODEC_AV1 };
struct avd_buf { void *cpu; dma_addr_t addr; size_t size; };
struct avd_frame { struct avd_buf *prog; size_t prog_words; };
struct avd_segment { size_t num; u32 instructions[AVD_MAX_INST]; };
struct avd_job { enum avd_codec codec; size_t num; struct avd_segment *segments; };
struct vb2_buffer { dma_addr_t addr; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; struct { u32 length; } planes[1]; };
struct avd_comp { u32 size; size_t start_offset; u32 offsets[4]; };
struct avd_decoded_buffer {
	struct { struct vb2_v4l2_buffer vb; } base;
	struct avd_comp comp;
	struct { bool is_intra; } hevc;
};
struct avd_variant { unsigned int quirks; unsigned char revision; };
struct avd_dev { const struct avd_variant *variant; };
struct test_format { struct { struct { u32 width, height; struct { u32 bytesperline; } plane_fmt[1]; } pix_mp; } fmt; };
struct avd_ctx {
	struct avd_dev *dev;
	void *priv;
	bool decomp;
	struct avd_comp comp;
	struct avd_job job;
	struct test_format decoded_fmt;
};
struct avd_run {
	struct { struct vb2_v4l2_buffer *src, *dst; } bufs;
	dma_addr_t coded_in, y_out, uv_out, comp_out;
};
static struct avd_decoded_buffer test_reference;
static inline struct avd_decoded_buffer *vb2_to_avd_decoded_buf(struct vb2_buffer *v)
{
	return (struct avd_decoded_buffer *)v;
}
static inline struct avd_decoded_buffer *avd_get_ref_buf(struct avd_ctx *ctx,
				struct vb2_v4l2_buffer *dst, u64 timestamp)
{
	(void)ctx; (void)dst; (void)timestamp;
	return &test_reference;
}
static inline dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *v, unsigned int plane)
{
	(void)plane;
	return v->addr;
}
static inline u32 fmt_width(struct avd_ctx *ctx) { return ctx->decoded_fmt.fmt.pix_mp.width; }
static inline u32 fmt_height(struct avd_ctx *ctx) { return ctx->decoded_fmt.fmt.pix_mp.height; }
#include "avd-inst.h"
#endif
