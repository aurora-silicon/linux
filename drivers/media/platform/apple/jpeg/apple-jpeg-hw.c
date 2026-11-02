// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Silicon JPEG engine: encode job programming, without hardware access
 * so that KUnit covers all of it.
 *
 * Quantisation tables are the ITU-T T.81 Annex K tables scaled with the
 * libjpeg quality formula.
 */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>

#include <kunit/visibility.h>

#include "apple-jpeg-hw.h"

/* T.81 Annex K, tables K.1 and K.2, in natural (row-major) order. */
static const u8 apple_jpeg_annex_k_luma[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99,
};

static const u8 apple_jpeg_annex_k_chroma[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
};

void apple_jpeg_quant_tables(u32 quality, u8 luma[64], u8 chroma[64])
{
	u32 q = clamp_t(u32, quality, 1, 100);
	u32 scale = q < 50 ? 5000 / q : 200 - 2 * q;

	for (unsigned int i = 0; i < 64; i++) {
		luma[i] = clamp_t(u32, (apple_jpeg_annex_k_luma[i] * scale + 50) / 100, 1, 255);
		chroma[i] = clamp_t(u32, (apple_jpeg_annex_k_chroma[i] * scale + 50) / 100, 1, 255);
	}
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_quant_tables);

/* Engine cycles allowed per job: three per input byte, within fixed bounds. */
u32 apple_jpeg_timeout(u32 width, u32 height)
{
	u64 t = 3ULL * width * height;

	return clamp_t(u64, t, 0x03000000, 0x7ed6b400);
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_timeout);

struct aj_list {
	struct apple_jpeg_write *w;
	unsigned int n, max;
};

static void aj_put(struct aj_list *l, u32 offset, u32 value)
{
	if (l->n < l->max)
		l->w[l->n] = (struct apple_jpeg_write){ offset, value };
	l->n++;
}

static void aj_put64(struct aj_list *l, u32 offset, u64 value)
{
	aj_put(l, offset, lower_32_bits(value));
	aj_put(l, offset + 4, upper_32_bits(value));
}

static void aj_put_range(struct aj_list *l, u32 from, u32 to, u32 value)
{
	for (u32 off = from; off <= to; off += 4)
		aj_put(l, off, value);
}

static void aj_defaults(struct aj_list *l, bool encode)
{
	static const u32 zero[] = {
		0x000, 0x004, 0x028, 0x02c, 0x030, 0x090, 0x094, 0x0a0, 0x0a4,
		0x0c0, 0x0c4, 0x0d0, 0x0d4, 0x1c0, 0x1dc, 0x288, 0x28c, 0x290,
		0x2a4, 0x2a8, 0x2ac,
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(zero); i++)
		aj_put(l, zero[i], 0);
	aj_put_range(l, 0x03c, 0x058, 0);
	aj_put_range(l, 0x0e8, 0x110, 0);
	aj_put_range(l, 0x13c, 0x158, 0);
	aj_put_range(l, 0x16c, 0x198, 0);
	aj_put_range(l, 0x1e4, 0x1f0, 0);
	aj_put_range(l, 0x1f8, 0x210, 0);
	aj_put_range(l, 0x218, 0x254, 0);
	aj_put_range(l, 0x260, 0x27c, 0);
	aj_put(l, AJ_DECODE_ENABLE, 1);
	aj_put(l, AJ_ENCODE_ENABLE, 1);
	aj_put_range(l, 0x05c, 0x088, 1);
	aj_put64(l, AJ_INPUT_END, 0xfffffffffULL);
	aj_put64(l, AJ_OUTPUT_END, 0xfffffffffULL);
	aj_put(l, AJ_ENCODE_PIPE_LIMIT0, 1);
	aj_put(l, AJ_ENCODE_PIPE_LIMIT1, 1);
	aj_put(l, 0x280, 0x1f40);
	aj_put(l, 0x294, 7);
	aj_put(l, 0x2b0, 7);
	aj_put(l, AJ_IRQ_MASK, 0xff);
	aj_put(l, AJ_TILING_ENABLE, encode);
}

/* The reset image, as written between the mode changes of a reset. */
int apple_jpeg_defaults(bool encode, struct apple_jpeg_write *w, unsigned int max)
{
	struct aj_list l = { w, 0, max };

	aj_defaults(&l, encode);
	return l.n <= max ? l.n : -ENOSPC;
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_defaults);

/*
 * Every register write of one baseline encode of two-plane (NV12 or NV16)
 * full-range input, in order, ending with the start command.
 */
int apple_jpeg_build_job(const struct apple_jpeg_job *job, struct apple_jpeg_write *w,
			 unsigned int max)
{
	u32 width = job->width, height = job->height;
	u32 mcu_h = job->is422 ? 8 : 16;
	u32 crows = job->is422 ? height : DIV_ROUND_UP(height, 2);
	struct aj_list l = { w, 0, max };
	u8 luma[64], chroma[64];

	if (!width || !height || width > U16_MAX || height > U16_MAX)
		return -EINVAL;
	if (job->luma_stride < width || job->chroma_stride < ALIGN(width, 2))
		return -EINVAL;
	if ((job->luma | job->chroma) & 3 || job->output & 31 || !job->output_size)
		return -EINVAL;
	if (job->input_end < job->luma || job->input_end < job->chroma ||
	    job->restart_interval > U16_MAX)
		return -EINVAL;

	aj_put(&l, AJ_MODE, AJ_MODE_SETUP);
	aj_put(&l, AJ_MAIN_RUN, 0);
	aj_defaults(&l, true);
	aj_put(&l, AJ_MODE, AJ_MODE_OPEN);

	aj_put(&l, AJ_ENCODE_ENABLE, 1);
	aj_put(&l, AJ_CODEC_MODE, 1);
	aj_put(&l, AJ_DECODE_ENABLE, 0);
	aj_put(&l, AJ_CODEC, job->is422 ? AJ_CODEC_422 : AJ_CODEC_420);

	aj_put(&l, AJ_PX_USE_PLANE1, 1);
	aj_put(&l, AJ_PX_MCUS_W, DIV_ROUND_UP(width, 16));
	aj_put(&l, AJ_PX_MCUS_H, DIV_ROUND_UP(height, mcu_h));
	aj_put(&l, AJ_PX_PLANE0_WIDTH_M1, width - 1);
	aj_put(&l, AJ_PX_PLANE0_HEIGHT_M1, height - 1);
	aj_put(&l, AJ_PX_PLANE0_TILE_H, 2);
	aj_put(&l, AJ_PX_PLANE0_TILE_V, mcu_h);
	aj_put(&l, AJ_PX_PLANE0_STRIDE, job->luma_stride);
	aj_put(&l, AJ_PX_PLANE1_WIDTH_M1, ALIGN(width, 2) - 1);
	aj_put(&l, AJ_PX_PLANE1_HEIGHT_M1, crows - 1);
	aj_put(&l, AJ_PX_PLANE1_TILE_H, 2);
	aj_put(&l, AJ_PX_PLANE1_TILE_V, 8);
	aj_put(&l, AJ_PX_PLANE1_STRIDE, job->chroma_stride);
	aj_put(&l, AJ_DMA_IMAGE_WIDTH, width);
	aj_put(&l, AJ_DMA_IMAGE_HEIGHT, height);

	aj_put64(&l, AJ_INPUT_PLANE0, job->luma);
	aj_put64(&l, AJ_INPUT_PLANE1, job->chroma);
	/* The engine treats the end as inclusive of a whole 8-byte word. */
	aj_put64(&l, AJ_INPUT_END, job->input_end | 7);
	aj_put64(&l, AJ_OUTPUT_START0, job->output);
	aj_put64(&l, AJ_OUTPUT_START1, job->output);
	aj_put64(&l, AJ_OUTPUT_END, job->output + job->output_size - 1);
	aj_put(&l, AJ_ENCODE_PIPE_LIMIT0, job->is422 ? 8 : 3);
	aj_put(&l, AJ_ENCODE_PIPE_LIMIT1, job->is422 ? 2 : 1);

	/* Two planes, Y then interleaved Cb, Cr; no colour conversion. */
	aj_put(&l, AJ_ENCODE_PIXEL_FORMAT, AJ_PIXEL_FORMAT_YUV_PLANES);
	aj_put(&l, AJ_ENCODE_COMPONENT_POS(0), 0);
	aj_put(&l, AJ_ENCODE_COMPONENT_POS(1), 0);
	aj_put(&l, AJ_ENCODE_COMPONENT_POS(2), 1);
	aj_put(&l, AJ_ENCODE_COMPONENT_POS(3), 0);
	aj_put(&l, AJ_CONVERT_COLOR_SPACE, 0);

	aj_put(&l, AJ_TIMEOUT, apple_jpeg_timeout(width, height));
	aj_put(&l, AJ_AXI_READ_PREFETCH, 1);
	aj_put(&l, AJ_AXI_WRITE_PREFETCH, 0);

	aj_put(&l, AJ_MODE, AJ_MODE_ENCODE);
	aj_put(&l, AJ_JPEG_IO_FLAGS, AJ_IO_FLAGS_ORDER |
	       (job->is422 ? AJ_IO_FLAGS_422 : AJ_IO_FLAGS_420));
	aj_put(&l, AJ_JPEG_WIDTH, width);
	aj_put(&l, AJ_JPEG_HEIGHT, height);
	aj_put(&l, AJ_RST_INTERVAL, job->restart_interval);
	aj_put(&l, AJ_OUTPUT_FLAGS, 0);

	apple_jpeg_quant_tables(job->quality, luma, chroma);
	for (unsigned int i = 0; i < 16; i++) {
		aj_put(&l, AJ_QTBL(0) + 4 * i, luma[4 * i] << 24 | luma[4 * i + 1] << 16 |
		       luma[4 * i + 2] << 8 | luma[4 * i + 3]);
		aj_put(&l, AJ_QTBL(1) + 4 * i, chroma[4 * i] << 24 | chroma[4 * i + 1] << 16 |
		       chroma[4 * i + 2] << 8 | chroma[4 * i + 3]);
	}
	aj_put(&l, AJ_QTBL_SEL, AJ_QTBL_SEL_LUMA_CHROMA);
	aj_put(&l, AJ_HUFFMAN_TABLE, AJ_HUFFMAN_BASELINE);

	aj_put(&l, AJ_MAIN_RUN, 1);
	aj_put(&l, AJ_COMMAND, 1);
	return l.n <= max ? l.n : -ENOSPC;
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_build_job);
